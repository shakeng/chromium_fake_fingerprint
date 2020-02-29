// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/passwords/password_controller.h"

#import <Foundation/Foundation.h>

#include <memory>
#include <utility>

#include "base/json/json_reader.h"
#include "base/memory/ref_counted.h"
#include "base/stl_util.h"
#include "base/strings/sys_string_conversions.h"
#include "base/strings/utf_string_conversions.h"
#import "base/test/ios/wait_util.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/task_environment.h"
#include "base/values.h"
#include "components/autofill/core/browser/logging/log_buffer_submitter.h"
#include "components/autofill/core/browser/logging/log_manager.h"
#include "components/autofill/core/common/password_form_fill_data.h"
#include "components/password_manager/core/browser/mock_password_store.h"
#include "components/password_manager/core/browser/password_form_manager.h"
#include "components/password_manager/core/browser/password_store_consumer.h"
#include "components/password_manager/core/browser/stub_password_manager_client.h"
#include "components/password_manager/core/common/password_manager_pref_names.h"
#import "components/password_manager/ios/js_password_manager.h"
#import "components/password_manager/ios/password_form_helper.h"
#include "components/password_manager/ios/test_helpers.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#import "ios/chrome/browser/autofill/form_suggestion_controller.h"
#include "ios/chrome/browser/browser_state/test_chrome_browser_state.h"
#import "ios/chrome/browser/passwords/password_form_filler.h"
#include "ios/chrome/browser/passwords/password_manager_features.h"
#import "ios/chrome/browser/ui/autofill/form_input_accessory/form_input_accessory_mediator.h"
#include "ios/chrome/browser/web/chrome_web_client.h"
#import "ios/chrome/browser/web/chrome_web_test.h"
#include "ios/web/public/js_messaging/web_frame.h"
#include "ios/web/public/js_messaging/web_frame_util.h"
#import "ios/web/public/js_messaging/web_frames_manager.h"
#import "ios/web/public/navigation/navigation_item.h"
#import "ios/web/public/navigation/navigation_manager.h"
#import "ios/web/public/test/fakes/test_web_state.h"
#import "ios/web/public/test/web_js_test.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/gtest_mac.h"
#include "testing/platform_test.h"
#import "third_party/ocmock/OCMock/OCMock.h"
#import "third_party/ocmock/OCMock/OCPartialMockObject.h"
#include "url/gurl.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

using autofill::FormData;
using autofill::FormFieldData;
using autofill::PasswordForm;
using autofill::PasswordFormFillData;
using password_manager::PasswordFormManagerForUI;
using password_manager::PasswordFormManager;
using password_manager::PasswordStoreConsumer;
using password_manager::prefs::kPasswordManagerOnboardingState;
using password_manager::prefs::kWasOnboardingFeatureCheckedBefore;
using password_manager::prefs::kPasswordLeakDetectionEnabled;
using test_helpers::SetPasswordFormFillData;
using testing::NiceMock;
using testing::Return;
using base::ASCIIToUTF16;
using base::SysUTF8ToNSString;
using base::test::ios::kWaitForActionTimeout;
using base::test::ios::kWaitForJSCompletionTimeout;
using base::test::ios::WaitUntilConditionOrTimeout;
using testing::WithArg;
using testing::_;

namespace {

class MockWebState : public web::TestWebState {
 public:
  MOCK_CONST_METHOD0(GetBrowserState, web::BrowserState*(void));
};

class MockPasswordManagerClient
    : public password_manager::StubPasswordManagerClient {
 public:
  explicit MockPasswordManagerClient(password_manager::PasswordStore* store)
      : store_(store) {
    prefs_ = std::make_unique<TestingPrefServiceSimple>();
    prefs_->registry()->RegisterIntegerPref(
        kPasswordManagerOnboardingState,
        static_cast<int>(
            password_manager::metrics_util::OnboardingState::kDoNotShow));
    prefs_->registry()->RegisterBooleanPref(kWasOnboardingFeatureCheckedBefore,
                                            false);
    prefs_->registry()->RegisterBooleanPref(kPasswordLeakDetectionEnabled,
                                            true);
  }

  ~MockPasswordManagerClient() override = default;

  MOCK_CONST_METHOD0(GetLogManager, autofill::LogManager*(void));
  MOCK_CONST_METHOD0(IsIncognito, bool());
  MOCK_METHOD1(PromptUserToSaveOrUpdatePasswordPtr,
               void(PasswordFormManagerForUI*));
  MOCK_CONST_METHOD1(IsSavingAndFillingEnabled, bool(const GURL&));

  PrefService* GetPrefs() const override { return prefs_.get(); }

  password_manager::PasswordStore* GetProfilePasswordStore() const override {
    return store_;
  }

  // Workaround for std::unique_ptr<> lacking a copy constructor.
  bool PromptUserToSaveOrUpdatePassword(
      std::unique_ptr<PasswordFormManagerForUI> manager,
      bool update_password) override {
    PromptUserToSaveOrUpdatePasswordPtr(manager.release());
    return false;
  }

 private:
  std::unique_ptr<TestingPrefServiceSimple> prefs_;
  password_manager::PasswordStore* const store_;
};

ACTION_P(SaveToScopedPtr, scoped) {
  scoped->reset(arg0);
}

class MockLogManager : public autofill::LogManager {
 public:
  MOCK_CONST_METHOD1(LogTextMessage, void(const std::string& text));
  MOCK_CONST_METHOD1(LogEntry, void(base::Value&&));
  MOCK_CONST_METHOD0(IsLoggingActive, bool(void));

  // Methods not important for testing.
  void OnLogRouterAvailabilityChanged(bool router_can_be_used) override {}
  void SetSuspended(bool suspended) override {}
  autofill::LogBufferSubmitter Log() override {
    return autofill::LogBufferSubmitter(nullptr, false);
  }
};

// Creates PasswordController with the given |web_state| and a mock client
// using the given |store|. If not null, |weak_client| is filled with a
// non-owning pointer to the created client. The created controller is
// returned.
PasswordController* CreatePasswordController(
    web::WebState* web_state,
    password_manager::PasswordStore* store,
    MockPasswordManagerClient** weak_client) {
  auto client = std::make_unique<NiceMock<MockPasswordManagerClient>>(store);
  if (weak_client)
    *weak_client = client.get();
  return [[PasswordController alloc] initWithWebState:web_state
                                               client:std::move(client)];
}

PasswordForm CreatePasswordForm(const char* origin_url,
                                const char* username_value,
                                const char* password_value) {
  PasswordForm form;
  form.scheme = PasswordForm::Scheme::kHtml;
  form.origin = GURL(origin_url);
  form.signon_realm = origin_url;
  form.username_value = ASCIIToUTF16(username_value);
  form.password_value = ASCIIToUTF16(password_value);
  return form;
}

// Invokes the password store consumer with a single copy of |form|.
ACTION_P(InvokeConsumer, form) {
  std::vector<std::unique_ptr<PasswordForm>> result;
  result.push_back(std::make_unique<PasswordForm>(form));
  arg0->OnGetPasswordStoreResults(std::move(result));
}

ACTION(InvokeEmptyConsumerWithForms) {
  arg0->OnGetPasswordStoreResults(std::vector<std::unique_ptr<PasswordForm>>());
}

}  // namespace

@interface PasswordController (
    Testing)<CRWWebStateObserver, FormSuggestionProvider>

// Provides access to common form helper logic for testing with mocks.
@property(readonly) PasswordFormHelper* formHelper;

- (void)fillPasswordForm:(const PasswordFormFillData&)formData
       completionHandler:(void (^)(BOOL))completionHandler;

- (void)onNoSavedCredentials;

@end

@interface PasswordFormHelper (Testing)

// Provides access to JavaScript Manager for testing with mocks.
@property(readonly) JsPasswordManager* jsPasswordManager;

- (void)findPasswordFormsWithCompletionHandler:
    (void (^)(const std::vector<PasswordForm>&))completionHandler;

@end

// Real FormSuggestionController is wrapped to register the addition of
// suggestions.
@interface PasswordsTestSuggestionController : FormSuggestionController

@property(nonatomic, copy) NSArray* suggestions;

@end

@implementation PasswordsTestSuggestionController

@synthesize suggestions = _suggestions;

- (void)updateKeyboardWithSuggestions:(NSArray*)suggestions {
  self.suggestions = suggestions;
}


@end

class PasswordControllerTest : public ChromeWebTest {
 public:
  PasswordControllerTest()
      : ChromeWebTest(std::make_unique<ChromeWebClient>()),
        store_(new testing::NiceMock<password_manager::MockPasswordStore>()) {}

  ~PasswordControllerTest() override { store_->ShutdownOnUIThread(); }

  void SetUp() override {
    ChromeWebTest::SetUp();
    ON_CALL(*store_, IsAbleToSavePasswords()).WillByDefault(Return(true));

    // When waiting for predictions is on, it makes tests more complicated.
    // Disable wating, since most tests have nothing to do with predictions. All
    // tests that test working with prediction should explicitly turn
    // predictions on.
    PasswordFormManager::set_wait_for_server_predictions_for_filling(false);

    passwordController_ =
        CreatePasswordController(web_state(), store_.get(), &weak_client_);

    ON_CALL(*weak_client_, IsSavingAndFillingEnabled(_))
        .WillByDefault(Return(true));

    @autoreleasepool {
      // Make sure the temporary array is released after SetUp finishes,
      // otherwise [passwordController_ suggestionProvider] will be retained
      // until PlatformTest teardown, at which point all Chrome objects are
      // already gone and teardown may access invalid memory.
      suggestionController_ = [[PasswordsTestSuggestionController alloc]
          initWithWebState:web_state()
                 providers:@[ [passwordController_ suggestionProvider] ]];
      accessoryMediator_ =
          [[FormInputAccessoryMediator alloc] initWithConsumer:nil
                                                      delegate:nil
                                                  webStateList:NULL
                                           personalDataManager:NULL
                                                 passwordStore:nullptr];
      [accessoryMediator_ injectWebState:web_state()];
      [accessoryMediator_ injectProvider:suggestionController_];
    }
  }

 protected:
  // Helper method for PasswordControllerTest.DontFillReadonly. Tries to load
  // |html| and find and fill there a form with hard-coded form data. Returns
  // YES on success, NO otherwise.
  BOOL BasicFormFill(NSString* html);

  // Retrieve the current suggestions from suggestionController_.
  NSArray* GetSuggestionValues() {
    NSMutableArray* suggestion_values = [NSMutableArray array];
    for (FormSuggestion* suggestion in [suggestionController_ suggestions])
      [suggestion_values addObject:suggestion.value];
    return [suggestion_values copy];
  }

  // Returns an identifier for the |form_number|th form in the page.
  std::string FormName(int form_number) {
    NSString* kFormNamingScript =
        @"__gCrWeb.form.getFormIdentifier("
         "    document.querySelectorAll('form')[%d]);";
    return base::SysNSStringToUTF8(ExecuteJavaScript(
        [NSString stringWithFormat:kFormNamingScript, form_number]));
  }

  // Sets up a partial mock that intercepts calls to the selector
  // -fillPasswordForm:withUsername:password:completionHandler: to the
  // PasswordController's JavaScript manager. For the first
  // |target_failure_count| calls, skips the invocation of the real JavaScript
  // manager, giving the effect that password form fill failed. As soon as
  // |failure_count| reaches |target_failure_count|, stop the partial mock
  // and let the original JavaScript manager execute.
  void SetFillPasswordFormFailureCount(int target_failure_count) {
    id original_manager = passwordController_.formHelper.jsPasswordManager;
    OCPartialMockObject* failing_manager =
        [OCMockObject partialMockForObject:original_manager];
    __block int failure_count = 0;
    void (^fail_invocation)(NSInvocation*) = ^(NSInvocation* invocation) {
      if (failure_count >= target_failure_count) {
        [failing_manager stopMocking];
        [invocation invokeWithTarget:original_manager];
      } else {
        ++failure_count;
        // Fetches the completion handler from |invocation| and calls it with
        // failure status.
        __unsafe_unretained void (^completionHandler)(BOOL);
        const NSInteger kArgOffset = 1;
        const NSInteger kCompletionHandlerArgIndex = 4;
        [invocation getArgument:&completionHandler
                        atIndex:(kCompletionHandlerArgIndex + kArgOffset)];
        ASSERT_TRUE(completionHandler);
        completionHandler(NO);
      }
    };
    [[[failing_manager stub] andDo:fail_invocation]
         fillPasswordForm:[OCMArg any]
             withUsername:[OCMArg any]
                 password:[OCMArg any]
        completionHandler:[OCMArg any]];
  }

  // SuggestionController for testing.
  PasswordsTestSuggestionController* suggestionController_;

  // FormInputAccessoryMediatorfor testing.
  FormInputAccessoryMediator* accessoryMediator_;

  // PasswordController for testing.
  PasswordController* passwordController_;

  scoped_refptr<password_manager::MockPasswordStore> store_;

  MockPasswordManagerClient* weak_client_;
};

struct FindPasswordFormTestData {
  NSString* html_string;
  const bool expected_form_found;
  // Expected number of fields in found form.
  const size_t expected_number_of_fields;
  // Expected form name.
  const char* expected_form_name;
};

FormData MakeSimpleFormData() {
  FormData form_data;
  form_data.url = GURL("http://www.google.com/a/LoginAuth");
  form_data.action = GURL("http://www.google.com/a/Login");
  form_data.name = ASCIIToUTF16("login_form");

  FormFieldData field;
  field.name = ASCIIToUTF16("Username");
  field.id_attribute = field.name;
  field.name_attribute = field.name;
  field.value = ASCIIToUTF16("googleuser");
  field.form_control_type = "text";
  field.unique_id = field.id_attribute;
  form_data.fields.push_back(field);

  field.name = ASCIIToUTF16("Passwd");
  field.id_attribute = field.name;
  field.name_attribute = field.name;
  field.value = ASCIIToUTF16("p4ssword");
  field.form_control_type = "password";
  field.unique_id = field.id_attribute;
  form_data.fields.push_back(field);

  return form_data;
}

PasswordForm MakeSimpleForm() {
  PasswordForm form;
  form.origin = GURL("http://www.google.com/a/LoginAuth");
  form.action = GURL("http://www.google.com/a/Login");
  form.username_element = ASCIIToUTF16("Username");
  form.password_element = ASCIIToUTF16("Passwd");
  form.username_value = ASCIIToUTF16("googleuser");
  form.password_value = ASCIIToUTF16("p4ssword");
  form.signon_realm = "http://www.google.com/";
  form.form_data = MakeSimpleFormData();
  return form;
}

// TODO(crbug.com/403705) This test is flaky.
// Check that HTML forms are converted correctly into FormDatas.
TEST_F(PasswordControllerTest, FLAKY_FindPasswordFormsInView) {
  // clang-format off
  FindPasswordFormTestData test_data[] = {
     // Normal form: a username and a password element.
    {
      @"<form name='form1'>"
      "<input type='text' name='user0'>"
      "<input type='password' name='pass0'>"
      "</form>",
      true, 2, "form1"
    },
    // User name is captured as an email address (HTML5).
    {
      @"<form name='form1'>"
      "<input type='email' name='email1'>"
      "<input type='password' name='pass1'>"
      "</form>",
      true, 2, "form1"
    },
    // No form found.
    {
      @"<div>",
      false, 0, nullptr
    },
    // Disabled username element.
    {
      @"<form name='form1'>"
      "<input type='text' name='user2' disabled='disabled'>"
      "<input type='password' name='pass2'>"
      "</form>",
      true, 2, "form1"
    },
    // No password element.
    {
      @"<form name='form1'>"
      "<input type='text' name='user3'>"
      "</form>",
      false, 0, nullptr
    },
  };
  // clang-format on

  for (const FindPasswordFormTestData& data : test_data) {
    SCOPED_TRACE(testing::Message() << "for html_string=" << data.html_string);
    LoadHtml(data.html_string);
    __block std::vector<FormData> forms;
    __block BOOL block_was_called = NO;
    [passwordController_.formHelper findPasswordFormsWithCompletionHandler:^(
                                        const std::vector<FormData>& result) {
      block_was_called = YES;
      forms = result;
    }];
    EXPECT_TRUE(
        WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^bool() {
          return block_was_called;
        }));
    if (data.expected_form_found) {
      ASSERT_EQ(1U, forms.size());
      EXPECT_EQ(data.expected_number_of_fields, forms[0].fields.size());
      EXPECT_EQ(data.expected_form_name, base::UTF16ToUTF8(forms[0].name));
    } else {
      ASSERT_TRUE(forms.empty());
    }
  }
}

// Test HTML page.  It contains several password forms.  Tests autofill
// them and verify that the right ones are autofilled.
static NSString* kHtmlWithMultiplePasswordForms =
    @""
     // Basic form.
     "<form>"
     "<input id='un0' type='text' name='u0'>"
     "<input id='pw0' type='password' name='p0'>"
     "</form>"
     // Form with action in the same origin.
     "<form action='?query=yes#reference'>"
     "<input id='un1' type='text' name='u1'>"
     "<input id='pw1' type='password' name='p1'>"
     "</form>"
     // Form with two exactly same password fields.
     "<form>"
     "<input id='un2' type='text' name='u2'>"
     "<input id='pw2' type='password' name='p2'>"
     "<input id='pw2' type='password' name='p2'>"
     "</form>"
     // Forms with same names but different ids (1 of 2).
     "<form>"
     "<input id='un3' type='text' name='u3'>"
     "<input id='pw3' type='password' name='p3'>"
     "</form>"
     // Forms with same names but different ids (2 of 2).
     "<form>"
     "<input id='un4' type='text' name='u4'>"
     "<input id='pw4' type='password' name='p4'>"
     "</form>"
     // Basic form, but with quotes in the names and IDs.
     "<form name=\"f5'\">"
     "<input id=\"un5'\" type='text' name=\"u5'\">"
     "<input id=\"pw5'\" type='password' name=\"p5'\">"
     "</form>"
     // Test forms inside iframes.
     "<iframe id='pf' name='pf'></iframe>"
     "<iframe id='npf' name='npf'></iframe>"
     "<script>"
     "  var doc = frames['pf'].document.open();"
     // Add a form inside iframe. It should also be matched and autofilled.
     "  doc.write('<form id=\\'f10\\'><input id=\\'un10\\' type=\\'text\\' "
     "name=\\'u10\\'>');"
     "  doc.write('<input id=\\'pw10\\' type=\\'password\\' name=\\'p10\\'>');"
     "  doc.write('</form>');"
     // Add a non-password form inside iframe. It should not be matched.
     "  var doc = frames['npf'].document.open();"
     "  doc.write('<form id=\\'f10\\'><input id=\\'un10\\' type=\\'text\\' "
     "name=\\'u10\\'>');"
     "  doc.write('<input id=\\'pw10\\' type=\\'text\\' name=\\'p10\\'>');"
     "  doc.write('</form>');"
     "  doc.close();"
     "</script>"
     // Fields inside this form don't have name.
     "<form>"
     "<input id='un6' type='text'>"
     "<input id='pw6' type='password'>"
     "</form>"
     // Fields in this form is attached by form's id.
     "<form id='form7'></form>"
     "<input id='un7' type='text' form='form7'>"
     "<input id='pw7' type='password' form='form7'>"
     // Fields that are outside the <form> tag.
     "<input id='un8' type='text'>"
     "<input id='pw8' type='password'>";

// A script that resets all text fields, including those in iframes.
static NSString* kClearInputFieldsScript =
    @"function clearInputFields(win) {"
     "  var inputs = win.document.getElementsByTagName('input');"
     "  for (var i = 0; i < inputs.length; i++) {"
     "    inputs[i].value = '';"
     "  }"
     "  var frames = win.frames;"
     "  for (var i = 0; i < frames.length; i++) {"
     "    clearInputFields(frames[i]);"
     "  }"
     "}"
     "clearInputFields(window);";

// A script that runs after autofilling forms.  It returns ids and values of all
// non-empty fields, including those in iframes.
static NSString* kInputFieldValueVerificationScript =
    @"function findAllInputsInFrame(win, prefix) {"
     "  var result = '';"
     "  var inputs = win.document.getElementsByTagName('input');"
     "  for (var i = 0; i < inputs.length; i++) {"
     "    var input = inputs[i];"
     "    if (input.value) {"
     "      result += prefix + input.id + '=' + input.value + ';';"
     "    }"
     "  }"
     "  var frames = win.frames;"
     "  for (var i = 0; i < frames.length; i++) {"
     "    result += findAllInputsInFrame("
     "        frames[i], prefix + frames[i].name +'.');"
     "  }"
     "  return result;"
     "};"
     "function findAllInputs(win) {"
     "  return findAllInputsInFrame(win, '');"
     "};"
     "var result = findAllInputs(window); result";

struct FillPasswordFormTestData {
  const std::string origin;
  const char* name;
  const char* username_field;
  const char* username_value;
  const char* password_field;
  const char* password_value;
  const BOOL should_succeed;
  // Expected result generated by |kInputFieldValueVerificationScript|.
  NSString* expected_result;
};

// Tests that filling password forms works correctly.
TEST_F(PasswordControllerTest, FillPasswordForm) {
  LoadHtml(kHtmlWithMultiplePasswordForms);

  const std::string base_url = BaseUrl();
  // clang-format off
  FillPasswordFormTestData test_data[] = {
    // Basic test: one-to-one match on the first password form.
    {
      base_url,
      "gChrome~form~0",
      "un0",
      "test_user",
      "pw0",
      "test_password",
      YES,
      @"un0=test_user;pw0=test_password;"
    },
    // The form matches despite a different action: the only difference
    // is a query and reference.
    {
      base_url,
      "gChrome~form~1",
      "un1",
      "test_user",
      "pw1",
      "test_password",
      YES,
      @"un1=test_user;pw1=test_password;"
    },
    // No match because of a different origin.
    {
      "http://someotherfakedomain.com",
      "gChrome~form~0",
      "un0",
      "test_user",
      "pw0",
      "test_password",
      NO,
      @""
    },
    // No match because some inputs are not in the form.
    {
      base_url,
      "gChrome~form~0",
      "un0",
      "test_user",
      "pw1",
      "test_password",
      NO,
      @""
    },
    // There are inputs with duplicate names in the form, the first of them is
    // filled.
    {
      base_url,
      "gChrome~form~2",
      "un2",
      "test_user",
      "pw2",
      "test_password",
      YES,
      @"un2=test_user;pw2=test_password;"
    },
    // Basic test, but with quotes in the names and IDs.
    {
      base_url,
      "f5'",
      "un5'",
      "test_user",
      "pw5'",
      "test_password",
      YES,
      @"un5'=test_user;pw5'=test_password;"
    },
    // Fields don't have name attributes so id attribute is used for fields
    // identification.
    {
      base_url,
      "gChrome~form~6",
      "un6",
      "test_user",
      "pw6",
      "test_password",
      YES,
      @"un6=test_user;pw6=test_password;"
    },
    // Fields in this form is attached by form's id.
    {
      base_url,
      "form7",
      "un7",
      "test_user",
      "pw7",
      "test_password",
      YES,
      @"un7=test_user;pw7=test_password;"
    },
    // Filling forms inside iframes.
    {
      base_url,
      "f10",
      "un10",
      "test_user",
      "pw10",
      "test_password",
      YES,
      @"pf.un10=test_user;pf.pw10=test_password;"
    },
    // Fields outside the form tag.
    {
      base_url,
      "",
      "un8",
      "test_user",
      "pw8",
      "test_password",
      YES,
      @"un8=test_user;pw8=test_password;"
    },
  };
  // clang-format on

  for (const FillPasswordFormTestData& data : test_data) {
    ExecuteJavaScript(kClearInputFieldsScript);

    PasswordFormFillData form_data;
    SetPasswordFormFillData(data.origin, data.name, data.username_field,
                            data.username_value, data.password_field,
                            data.password_value, nullptr, nullptr, false,
                            &form_data);

    __block BOOL block_was_called = NO;
    [passwordController_ fillPasswordForm:form_data
                        completionHandler:^(BOOL success) {
                          block_was_called = YES;
                          EXPECT_EQ(data.should_succeed, success);
                        }];
    EXPECT_TRUE(
        WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^bool() {
          return block_was_called;
        }));

    id result = ExecuteJavaScript(kInputFieldValueVerificationScript);
    EXPECT_NSEQ(data.expected_result, result);
  }
}

// Tests that a form is found and the found form is filled in with the given
// username and password.
TEST_F(PasswordControllerTest, FindAndFillOnePasswordForm) {
  LoadHtml(@"<form><input id='un' type='text' name='u'>"
            "<input id='pw' type='password' name='p'></form>");
  __block int call_counter = 0;
  __block int success_counter = 0;
  [passwordController_.passwordFormFiller
      findAndFillPasswordForms:@"john.doe@gmail.com"
                      password:@"super!secret"
             completionHandler:^(BOOL complete) {
               ++call_counter;
               if (complete)
                 ++success_counter;
             }];
  EXPECT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return call_counter == 1;
  }));
  EXPECT_EQ(1, success_counter);
  id result = ExecuteJavaScript(kInputFieldValueVerificationScript);
  EXPECT_NSEQ(@"un=john.doe@gmail.com;pw=super!secret;", result);
}

// Tests that multiple forms on the same page are found and filled.
// This test includes an mock injected failure on form filling to verify
// that completion handler is called with the proper values.
TEST_F(PasswordControllerTest, FindAndFillMultiplePasswordForms) {
  // Fails the first call to fill password form.
  SetFillPasswordFormFailureCount(1);
  LoadHtml(@"<form><input id='u1' type='text' name='un1'>"
            "<input id='p1' type='password' name='pw1'></form>"
            "<form><input id='u2' type='text' name='un2'>"
            "<input id='p2' type='password' name='pw2'></form>"
            "<form><input id='u3' type='text' name='un3'>"
            "<input id='p3' type='password' name='pw3'></form>");
  __block int call_counter = 0;
  __block int success_counter = 0;
  [passwordController_.passwordFormFiller
      findAndFillPasswordForms:@"john.doe@gmail.com"
                      password:@"super!secret"
             completionHandler:^(BOOL complete) {
               ++call_counter;
               if (complete)
                 ++success_counter;
               LOG(INFO) << "HANDLER call " << call_counter << " success "
                         << success_counter;
             }];
  // There should be 3 password forms and only 2 successfully filled forms.
  EXPECT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return call_counter == 3;
  }));
  EXPECT_EQ(2, success_counter);
  id result = ExecuteJavaScript(kInputFieldValueVerificationScript);
  EXPECT_NSEQ(@"u2=john.doe@gmail.com;p2=super!secret;"
               "u3=john.doe@gmail.com;p3=super!secret;",
              result);
}

BOOL PasswordControllerTest::BasicFormFill(NSString* html) {
  LoadHtml(html);
  const std::string base_url = BaseUrl();
  PasswordFormFillData form_data;
  SetPasswordFormFillData(base_url, "gChrome~form~0", "un0", "test_user", "pw0",
                          "test_password", nullptr, nullptr, false, &form_data);
  __block BOOL block_was_called = NO;
  __block BOOL return_value = NO;
  [passwordController_ fillPasswordForm:form_data
                      completionHandler:^(BOOL success) {
                        block_was_called = YES;
                        return_value = success;
                      }];
  EXPECT_TRUE(WaitUntilConditionOrTimeout(kWaitForActionTimeout, ^bool() {
    return block_was_called;
  }));
  return return_value;
}

// Check that |fillPasswordForm| is not filled if 'readonly' attribute is set
// on either username or password fields.
// TODO(crbug.com/503050): Test is flaky.
TEST_F(PasswordControllerTest, FLAKY_DontFillReadOnly) {
  // Control check that the fill operation will succceed with well-formed form.
  EXPECT_TRUE(BasicFormFill(@"<form>"
                             "<input id='un0' type='text' name='u0'>"
                             "<input id='pw0' type='password' name='p0'>"
                             "</form>"));
  // Form fill should fail with 'readonly' attribute on username.
  EXPECT_FALSE(BasicFormFill(
      @"<form>"
       "<input id='un0' type='text' name='u0' readonly='readonly'>"
       "<input id='pw0' type='password' name='p0'>"
       "</form>"));
  // Form fill should fail with 'readonly' attribute on password.
  EXPECT_FALSE(BasicFormFill(
      @"<form>"
       "<input id='un0' type='text' name='u0'>"
       "<input id='pw0' type='password' name='p0' readonly='readonly'>"
       "</form>"));
}

// TODO(crbug.com/817755): Move them HTML const to separate HTML files.
// An HTML page without a password form.
static NSString* kHtmlWithoutPasswordForm =
    @"<h2>The rain in Spain stays <i>mainly</i> in the plain.</h2>";

// An HTML page containing one password form.  The username input field
// also has custom event handlers.  We need to verify that those event
// handlers are still triggered even though we override them with our own.
static NSString* kHtmlWithPasswordForm =
    @"<form>"
     "<input id='un' type='text' name=\"u'\""
     "  onkeyup='window.onKeyUpCalled_=true'"
     "  onchange='window.onChangeCalled_=true'>"
     "<input id='pw' type='password' name=\"p'\">"
     "</form>";

static NSString* kHtmlWithNewPasswordForm =
    @"<form>"
     "<input id='un' type='text' name=\"u'\" autocomplete=\"username\""
     "  onkeyup='window.onKeyUpCalled_=true'"
     "  onchange='window.onChangeCalled_=true'>"
     "<input id='pw' type='password' name=\"p'\" autocomplete=\"new-password\">"
     "</form>";

// An HTML page containing two password forms.
static NSString* kHtmlWithTwoPasswordForms =
    @"<form id='f1'>"
     "<input type='text' id='u1'"
     "  onkeyup='window.onKeyUpCalled_=true'"
     "  onchange='window.onChangeCalled_=true'>"
     "<input type='password' id='p1'>"
     "</form>"
     "<form id='f2'>"
     "<input type='text' id='u2'"
     "  onkeyup='window.onKeyUpCalled_=true'"
     "  onchange='window.onChangeCalled_=true'>"
     "<input type='password' id='p2'>"
     "</form>";

// A script that resets indicators used to verify that custom event
// handlers are triggered.  It also finds and the username and
// password fields and caches them for future verification.
static NSString* kUsernameAndPasswordTestPreparationScript =
    @"onKeyUpCalled_ = false;"
     "onChangeCalled_ = false;"
     "username_ = document.getElementById('%@');"
     "username_.__gCrWebAutofilled = 'false';"
     "password_ = document.getElementById('%@');"
     "password_.__gCrWebAutofilled = 'false';";

// A script that we run after autofilling forms.  It returns
// all values for verification as a single concatenated string.
static NSString* kUsernamePasswordVerificationScript =
    @"var value = username_.value;"
     "var from = username_.selectionStart;"
     "var to = username_.selectionEnd;"
     "value.substr(0, from) + '[' + value.substr(from, to) + ']'"
     "   + value.substr(to, value.length) + '=' + password_.value"
     "   + ', onkeyup=' + onKeyUpCalled_"
     "   + ', onchange=' + onChangeCalled_;";

// A script that adds a password form.
static NSString* kAddFormDynamicallyScript =
    @"var dynamicForm = document.createElement('form');"
     "dynamicForm.setAttribute('name', 'dynamic_form');"
     "var inputUsername = document.createElement('input');"
     "inputUsername.setAttribute('type', 'text');"
     "inputUsername.setAttribute('id', 'username');"
     "var inputPassword = document.createElement('input');"
     "inputPassword.setAttribute('type', 'password');"
     "inputPassword.setAttribute('id', 'password');"
     "var submitButton = document.createElement('input');"
     "submitButton.setAttribute('type', 'submit');"
     "submitButton.setAttribute('value', 'Submit');"
     "dynamicForm.appendChild(inputUsername);"
     "dynamicForm.appendChild(inputPassword);"
     "dynamicForm.appendChild(submitButton);"
     "document.body.appendChild(dynamicForm);";

struct SuggestionTestData {
  std::string description;
  NSArray* eval_scripts;
  NSArray* expected_suggestions;
  NSString* expected_result;
};

// Tests that form activity correctly sends suggestions to the suggestion
// controller.
TEST_F(PasswordControllerTest, SuggestionUpdateTests) {
  LoadHtml(kHtmlWithPasswordForm);
  const std::string base_url = BaseUrl();
  ExecuteJavaScript(
      [NSString stringWithFormat:kUsernameAndPasswordTestPreparationScript,
                                 @"un", @"pw"]);

  // Initialize |form_data| with test data and an indicator that autofill
  // should not be performed while the user is entering the username so that
  // we can test with an initially-empty username field. Testing with a
  // username field that contains input is performed by a specific test below.
  PasswordFormFillData form_data;
  SetPasswordFormFillData(base_url, "gChrome~form~0", "un", "user0", "pw",
                          "password0", "abc", "def", true, &form_data);

  __block BOOL block_was_called = NO;
  [passwordController_ fillPasswordForm:form_data
                      completionHandler:^(BOOL success) {
                        block_was_called = YES;
                        // Verify that the fill reports failed.
                        EXPECT_FALSE(success);
                      }];
  EXPECT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^bool() {
    return block_was_called;
  }));

  // Verify that the form has not been autofilled.
  EXPECT_NSEQ(@"[]=, onkeyup=false, onchange=false",
              ExecuteJavaScript(kUsernamePasswordVerificationScript));

  // clang-format off
  SuggestionTestData test_data[] = {
    {
      "Should show all suggestions when focusing empty username field",
      @[(@"var evt = document.createEvent('Events');"
         "username_.focus();"),
        @""],
      @[@"user0 ••••••••", @"abc ••••••••"],
      @"[]=, onkeyup=false, onchange=false"
    },
    {
      "Should show password suggestions when focusing password field",
      @[(@"var evt = document.createEvent('Events');"
         "password_.focus();"),
        @""],
      @[@"user0 ••••••••", @"abc ••••••••"],
      @"[]=, onkeyup=false, onchange=false"
    },
    {
      "Should not filter suggestions when focusing username field with input",
      @[(@"username_.value='ab';"
         "username_.focus();"),
        @""],
      @[@"user0 ••••••••", @"abc ••••••••"],
      @"ab[]=, onkeyup=false, onchange=false"
    },
  };
  // clang-format on

  for (const SuggestionTestData& data : test_data) {
    SCOPED_TRACE(testing::Message()
                 << "for description=" << data.description
                 << " and eval_scripts=" << data.eval_scripts);
    // Prepare the test.
    ExecuteJavaScript(
        [NSString stringWithFormat:kUsernameAndPasswordTestPreparationScript,
                                   @"un", @"pw"]);

    for (NSString* script in data.eval_scripts) {
      // Trigger events.
      ExecuteJavaScript(script);

      // Pump the run loop so that the host can respond.
      WaitForBackgroundTasks();
    }
    // Wait until suggestions are received.
    EXPECT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
      return [GetSuggestionValues() count] > 0;
    }));

    EXPECT_NSEQ(data.expected_suggestions, GetSuggestionValues());
    EXPECT_NSEQ(data.expected_result,
                ExecuteJavaScript(kUsernamePasswordVerificationScript));
    // Clear all suggestions.
    [suggestionController_ setSuggestions:nil];
  }
}

// Tests that selecting a suggestion will fill the corresponding form and field.
TEST_F(PasswordControllerTest, SelectingSuggestionShouldFillPasswordForm) {
  LoadHtml(kHtmlWithTwoPasswordForms);
  const std::string base_url = BaseUrl();

  struct TestData {
    const char* form_name;
    const char* username_element;
    const char* password_element;
  } const kTestData[] = {{"f1", "u1", "p1"}, {"f2", "u2", "p2"}};

  // Send fill data to passwordController_.
  for (size_t form_i = 0; form_i < base::size(kTestData); ++form_i) {
    // Initialize |form_data| with test data and an indicator that autofill
    // should not be performed while the user is entering the username so that
    // we can test with an initially-empty username field.
    const auto& test_data = kTestData[form_i];

    PasswordFormFillData form_data;
    SetPasswordFormFillData(base_url, test_data.form_name,
                            test_data.username_element, "user0",
                            test_data.password_element, "password0", "abc",
                            "def", true, &form_data);

    __block BOOL block_was_called = NO;
    [passwordController_ fillPasswordForm:form_data
                        completionHandler:^(BOOL success) {
                          block_was_called = YES;
                          // Verify that the fill reports failed.
                          EXPECT_FALSE(success);
                        }];
    EXPECT_TRUE(
        WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^bool() {
          return block_was_called;
        }));
  }

  // Check that the right password form is filled on suggesion selection.
  for (size_t form_i = 0; form_i < base::size(kTestData); ++form_i) {
    const auto& test_data = kTestData[form_i];
    NSString* form_name = SysUTF8ToNSString(test_data.form_name);
    NSString* username_element = SysUTF8ToNSString(test_data.username_element);
    NSString* password_element = SysUTF8ToNSString(test_data.password_element);

    // Prepare username and passwords for checking.
    ExecuteJavaScript(
        [NSString stringWithFormat:kUsernameAndPasswordTestPreparationScript,
                                   username_element, password_element]);

    // Verify that the form has not been autofilled.
    EXPECT_NSEQ(@"[]=, onkeyup=false, onchange=false",
                ExecuteJavaScript(kUsernamePasswordVerificationScript));

    std::string mainFrameID = web::GetMainWebFrameId(web_state());
    // Emulate that the user clicks on the username field in the first form.
    // That's required in order that PasswordController can identify which form
    // should be filled.
    __block BOOL block_was_called = NO;
    [passwordController_
        retrieveSuggestionsForForm:form_name
                   fieldIdentifier:username_element
                         fieldType:@"text"
                              type:@"focus"
                        typedValue:@""
                           frameID:SysUTF8ToNSString(mainFrameID)
                          webState:web_state()
                 completionHandler:^(NSArray* suggestions,
                                     id<FormSuggestionProvider> provider) {
                   NSMutableArray* suggestion_values = [NSMutableArray array];
                   for (FormSuggestion* suggestion in suggestions)
                     [suggestion_values addObject:suggestion.value];
                   EXPECT_NSEQ((@[
                                 @"user0 ••••••••", @"abc ••••••••",
                               ]),
                               suggestion_values);
                   block_was_called = YES;
                 }];
    EXPECT_TRUE(block_was_called);

    // Tell PasswordController that a suggestion was selected. It should fill
    // out the password form with the corresponding credentials.
    FormSuggestion* suggestion =
        [FormSuggestion suggestionWithValue:@"abc ••••••••"
                         displayDescription:nil
                                       icon:nil
                                 identifier:0];

    block_was_called = NO;
    SuggestionHandledCompletion completion = ^{
      block_was_called = YES;
      EXPECT_NSEQ(@"abc[]=def, onkeyup=true, onchange=true",
                  ExecuteJavaScript(kUsernamePasswordVerificationScript));
    };
    [passwordController_ didSelectSuggestion:suggestion
                                        form:SysUTF8ToNSString(FormName(0))
                             fieldIdentifier:@"u"
                                     frameID:SysUTF8ToNSString(mainFrameID)
                           completionHandler:completion];
    EXPECT_TRUE(
        WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^bool() {
          return block_was_called;
        }));
  }
}

using PasswordControllerTestSimple = PlatformTest;

// The test case below does not need the heavy fixture from above, but it
// needs to use MockWebState.
TEST_F(PasswordControllerTestSimple, SaveOnNonHTMLLandingPage) {
  base::test::TaskEnvironment task_environment;
  TestChromeBrowserState::Builder builder;
  std::unique_ptr<TestChromeBrowserState> browser_state(builder.Build());
  MockWebState web_state;
  id mock_js_injection_receiver =
      [OCMockObject mockForClass:[CRWJSInjectionReceiver class]];
  [[mock_js_injection_receiver expect] executeJavaScript:[OCMArg any]
                                       completionHandler:[OCMArg any]];
  web_state.SetJSInjectionReceiver(mock_js_injection_receiver);
  ON_CALL(web_state, GetBrowserState())
      .WillByDefault(testing::Return(browser_state.get()));

  MockPasswordManagerClient* weak_client = nullptr;
  PasswordController* passwordController =
      CreatePasswordController(&web_state, nullptr, &weak_client);

  // Use a mock LogManager to detect that OnPasswordFormsRendered has been
  // called. TODO(crbug.com/598672): this is a hack, we should modularize the
  // code better to allow proper unit-testing.
  MockLogManager log_manager;
  EXPECT_CALL(log_manager, IsLoggingActive()).WillRepeatedly(Return(true));
  EXPECT_CALL(
      log_manager,
      LogTextMessage("Message:  PasswordManager::OnPasswordFormsRendered \n"));
  EXPECT_CALL(log_manager,
              LogTextMessage(testing::Ne(
                  "Message:  PasswordManager::OnPasswordFormsRendered \n")))
      .Times(testing::AnyNumber());
  EXPECT_CALL(*weak_client, GetLogManager())
      .WillRepeatedly(Return(&log_manager));

  web_state.SetContentIsHTML(false);
  web_state.SetCurrentURL(GURL("https://example.com"));
  [passwordController webState:&web_state didLoadPageWithSuccess:YES];
}

// Checks that when the user set a focus on a field of a password form which was
// not sent to the store then the request the the store is sent.
TEST_F(PasswordControllerTest, SendingToStoreDynamicallyAddedFormsOnFocus) {
  LoadHtml(kHtmlWithoutPasswordForm);
  ExecuteJavaScript(kAddFormDynamicallyScript);

  // The standard pattern is to use a __block variable WaitUntilCondition but
  // __block variable can't be captured in C++ lambda, so as workaround it's
  // used normal variable |get_logins_called| and pointer on it is used in a
  // block.
  bool get_logins_called = false;
  bool* p_get_logins_called = &get_logins_called;

  password_manager::PasswordStore::FormDigest expected_form_digest(
      autofill::PasswordForm::Scheme::kHtml, "https://chromium.test/",
      GURL("https://chromium.test/"));
  // TODO(crbug.com/949519): replace WillRepeatedly with WillOnce when the old
  // parser is gone.
  EXPECT_CALL(*store_, GetLogins(expected_form_digest, _))
      .WillRepeatedly(testing::Invoke(
          [&get_logins_called](
              const password_manager::PasswordStore::FormDigest&,
              password_manager::PasswordStoreConsumer*) {
            get_logins_called = true;
          }));

  // Sets a focus on a username field.
  NSString* kSetUsernameInFocusScript =
      @"document.getElementById('username').focus();";
  ExecuteJavaScript(kSetUsernameInFocusScript);

  // Wait until GetLogins is called.
  EXPECT_TRUE(WaitUntilConditionOrTimeout(kWaitForActionTimeout, ^bool() {
    return *p_get_logins_called;
  }));
}

// Tests that a touchend event from a button which contains in a password form
// works as a submission indicator for this password form.
TEST_F(PasswordControllerTest, TouchendAsSubmissionIndicator) {
  const char* kHtml[] = {
      "<html><body>"
      "<form name='login_form' id='login_form'>"
      "  <input type='text' name='username'>"
      "  <input type='password' name='password'>"
      "  <button id='submit_button' value='Submit'>"
      "</form>"
      "</body></html>",
      "<html><body>"
      "<form name='login_form' id='login_form'>"
      "  <input type='text' name='username'>"
      "  <input type='password' name='password'>"
      "  <button id='back' value='Back'>"
      "  <button id='submit_button' type='submit' value='Submit'>"
      "</form>"
      "</body></html>"};

  MockLogManager log_manager;
  EXPECT_CALL(*weak_client_, GetLogManager())
      .WillRepeatedly(Return(&log_manager));

  for (size_t i = 0; i < base::size(kHtml); ++i) {
    LoadHtml(SysUTF8ToNSString(kHtml[i]));
    // Use a mock LogManager to detect that OnPasswordFormSubmitted has been
    // called. TODO(crbug.com/598672): this is a hack, we should modularize the
    // code better to allow proper unit-testing.
    EXPECT_CALL(log_manager, IsLoggingActive()).WillRepeatedly(Return(true));
    const char kExpectedMessage[] =
        "Message:  PasswordManager::ProvisionallySaveForm \n";
    EXPECT_CALL(log_manager, LogTextMessage(kExpectedMessage));
    EXPECT_CALL(log_manager, LogTextMessage(testing::Ne(kExpectedMessage)))
        .Times(testing::AnyNumber());

    ExecuteJavaScript(
        @"document.getElementsByName('username')[0].value = 'user1';"
         "document.getElementsByName('password')[0].value = 'password1';"
         "var e = new UIEvent('touchend');"
         "document.getElementById('submit_button').dispatchEvent(e);");
    testing::Mock::VerifyAndClearExpectations(&log_manager);
  }
}

// Tests that a touchend event from a button which contains in a password form
// works as a submission indicator for this password form.
TEST_F(PasswordControllerTest, SavingFromSameOriginIframe) {
  // Use a mock LogManager to detect that OnSameDocumentNavigation has been
  // called. TODO(crbug.com/598672): this is a hack, we should modularize the
  // code better to allow proper unit-testing.
  MockLogManager log_manager;
  EXPECT_CALL(*weak_client_, GetLogManager())
      .WillRepeatedly(Return(&log_manager));
  EXPECT_CALL(log_manager, IsLoggingActive()).WillRepeatedly(Return(true));
  const char kExpectedMessage[] =
      "Message:  PasswordManager::OnSameDocumentNavigation \n";

  // The standard pattern is to use a __block variable WaitUntilCondition but
  // __block variable can't be captured in C++ lambda, so as workaround it's
  // used normal variable |get_logins_called| and pointer on it is used in a
  // block.
  bool expected_message_logged = false;
  bool* p_expected_message_logged = &expected_message_logged;

  EXPECT_CALL(log_manager, LogTextMessage(kExpectedMessage))
      .WillOnce(testing::Invoke(
          [&expected_message_logged](const std::string& message) {
            expected_message_logged = true;
          }));

  EXPECT_CALL(log_manager, LogTextMessage(testing::Ne(kExpectedMessage)))
      .Times(testing::AnyNumber());

  LoadHtml(@"<iframe id='frame1' name='frame1'></iframe>");
  ExecuteJavaScript(
      @"document.getElementById('frame1').contentDocument.body.innerHTML = "
       "'<form id=\"form1\">"
       "<input type=\"text\" name=\"text\" value=\"user1\" id=\"id2\">"
       "<input type=\"password\" name=\"password\" value=\"pw1\" id=\"id2\">"
       "<input type=\"submit\" id=\"submit_input\"/>"
       "</form>'");
  ExecuteJavaScript(
      @"document.getElementById('frame1').contentDocument.getElementById('"
      @"submit_input').click();");

  // Wait until expected message is called.
  EXPECT_TRUE(WaitUntilConditionOrTimeout(kWaitForActionTimeout, ^bool() {
    return *p_expected_message_logged;
  }));
}

// Tests that when a dynamic form added and the user clicks on the username
// field in this form, then the request to the Password Store is sent and
// PassworController is waiting to the response in order to show or not to show
// password suggestions.
TEST_F(PasswordControllerTest, CheckAsyncSuggestions) {
  for (bool store_has_credentials : {false, true}) {
    LoadHtml(kHtmlWithoutPasswordForm);
    ExecuteJavaScript(kAddFormDynamicallyScript);

    __block BOOL completion_handler_success = NO;
    __block BOOL completion_handler_called = NO;

    if (store_has_credentials) {
      PasswordForm form(CreatePasswordForm(BaseUrl().c_str(), "user", "pw"));
      // TODO(crbug.com/949519): replace WillRepeatedly with WillOnce when the
      // old parser is gone.
      EXPECT_CALL(*store_, GetLogins(_, _))
          .WillRepeatedly(WithArg<1>(InvokeConsumer(form)));
    } else {
      EXPECT_CALL(*store_, GetLogins(_, _))
          .WillRepeatedly(WithArg<1>(InvokeEmptyConsumerWithForms()));
    }
    std::string mainFrameID = web::GetMainWebFrameId(web_state());
    [passwordController_
        checkIfSuggestionsAvailableForForm:@"dynamic_form"
                           fieldIdentifier:@"username"
                                 fieldType:@"text"
                                      type:@"focus"
                                typedValue:@""
                                   frameID:SysUTF8ToNSString(mainFrameID)
                               isMainFrame:YES
                            hasUserGesture:YES
                                  webState:web_state()
                         completionHandler:^(BOOL success) {
                           completion_handler_success = success;
                           completion_handler_called = YES;
                         }];
    // Wait until the expected handler is called.
    EXPECT_TRUE(WaitUntilConditionOrTimeout(kWaitForActionTimeout, ^bool() {
      return completion_handler_called;
    }));

    EXPECT_EQ(store_has_credentials, completion_handler_success);
    testing::Mock::VerifyAndClearExpectations(&store_);
  }
}

// Tests that when a dynamic form added and the user clicks on non username
// field in this form, then the request to the Password Store is sent but no
// suggestions are shown.
TEST_F(PasswordControllerTest, CheckNoAsyncSuggestionsOnNonUsernameField) {
  LoadHtml(kHtmlWithoutPasswordForm);
  ExecuteJavaScript(kAddFormDynamicallyScript);

  __block BOOL completion_handler_success = NO;
  __block BOOL completion_handler_called = NO;

  PasswordForm form(CreatePasswordForm(BaseUrl().c_str(), "user", "pw"));
  // TODO(crbug.com/949519): replace WillRepeatedly with WillOnce when the old
  // parser is gone.
  EXPECT_CALL(*store_, GetLogins(_, _))
      .WillRepeatedly(WithArg<1>(InvokeConsumer(form)));
  std::string mainFrameID = web::GetMainWebFrameId(web_state());
  [passwordController_
      checkIfSuggestionsAvailableForForm:@"dynamic_form"
                         fieldIdentifier:@"address"
                               fieldType:@"text"
                                    type:@"focus"
                              typedValue:@""
                                 frameID:SysUTF8ToNSString(mainFrameID)
                             isMainFrame:YES
                          hasUserGesture:YES
                                webState:web_state()
                       completionHandler:^(BOOL success) {
                         completion_handler_success = success;
                         completion_handler_called = YES;
                       }];
  // Wait until the expected handler is called.
  EXPECT_TRUE(WaitUntilConditionOrTimeout(kWaitForActionTimeout, ^bool() {
    return completion_handler_called;
  }));

  EXPECT_FALSE(completion_handler_success);
}

// Tests that when there are no password forms on a page and the user clicks on
// a text field the completion callback is called with no suggestions result.
TEST_F(PasswordControllerTest, CheckNoAsyncSuggestionsOnNoPasswordForms) {
  LoadHtml(kHtmlWithoutPasswordForm);

  __block BOOL completion_handler_success = NO;
  __block BOOL completion_handler_called = NO;

  EXPECT_CALL(*store_, GetLogins(_, _)).Times(0);
  std::string mainFrameID = web::GetMainWebFrameId(web_state());
  [passwordController_
      checkIfSuggestionsAvailableForForm:@"form"
                         fieldIdentifier:@"address"
                               fieldType:@"text"
                                    type:@"focus"
                              typedValue:@""
                                 frameID:SysUTF8ToNSString(mainFrameID)
                             isMainFrame:YES
                          hasUserGesture:YES
                                webState:web_state()
                       completionHandler:^(BOOL success) {
                         completion_handler_success = success;
                         completion_handler_called = YES;
                       }];
  // Wait until the expected handler is called.
  EXPECT_TRUE(WaitUntilConditionOrTimeout(kWaitForActionTimeout, ^bool() {
    return completion_handler_called;
  }));

  EXPECT_FALSE(completion_handler_success);
}

// Tests password generation suggestion is shown properly.
TEST_F(PasswordControllerTest, CheckPasswordGenerationSuggestion) {
  EXPECT_CALL(*store_, GetLogins(_, _))
      .WillRepeatedly(WithArg<1>(InvokeEmptyConsumerWithForms()));
  EXPECT_CALL(*weak_client_->GetPasswordFeatureManager(), IsGenerationEnabled())
      .WillRepeatedly(Return(true));

  LoadHtml(kHtmlWithNewPasswordForm);
  const std::string base_url = BaseUrl();
  ExecuteJavaScript(
      [NSString stringWithFormat:kUsernameAndPasswordTestPreparationScript,
                                 @"un", @"pw"]);

  // Initialize |form_data| with test data and an indicator that autofill
  // should not be performed while the user is entering the username so that
  // we can test with an initially-empty username field. Testing with a
  // username field that contains input is performed by a specific test below.
  PasswordFormFillData form_data;
  SetPasswordFormFillData(base_url, "gChrome~form~0", "un", "user0", "pw",
                          "password0", "abc", "def", true, &form_data);

  __block BOOL block_was_called = NO;
  [passwordController_ fillPasswordForm:form_data
                      completionHandler:^(BOOL success) {
                        block_was_called = YES;
                        // Verify that the fill reports failed.
                        EXPECT_FALSE(success);
                      }];
  EXPECT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^bool() {
    return block_was_called;
  }));

  // Verify that the form has not been autofilled.
  EXPECT_NSEQ(@"[]=, onkeyup=false, onchange=false",
              ExecuteJavaScript(kUsernamePasswordVerificationScript));

  // clang-format off
  SuggestionTestData test_data[] = {
    {
      "Should not show suggest password when focusing username field",
      @[(@"var evt = document.createEvent('Events');"
         "username_.focus();"),
        @""],
      @[@"user0 ••••••••", @"abc ••••••••"],
      @"[]=, onkeyup=false, onchange=false"
    },
    {
      "Should show suggest password when focusing password field",
      @[(@"var evt = document.createEvent('Events');"
         "password_.focus();"),
        @""],
      @[@"user0 ••••••••", @"abc ••••••••", @"Suggest  Password\u2026"],
      @"[]=, onkeyup=false, onchange=false"
    },
  };
  // clang-format on

  for (const SuggestionTestData& data : test_data) {
    SCOPED_TRACE(testing::Message()
                 << "for description=" << data.description
                 << " and eval_scripts=" << data.eval_scripts);
    // Prepare the test.
    ExecuteJavaScript(
        [NSString stringWithFormat:kUsernameAndPasswordTestPreparationScript,
                                   @"un", @"pw"]);

    for (NSString* script in data.eval_scripts) {
      // Trigger events.
      ExecuteJavaScript(script);

      // Pump the run loop so that the host can respond.
      WaitForBackgroundTasks();
    }
    // Wait until suggestions are received.
    EXPECT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
      return [GetSuggestionValues() count] > 0;
    }));

    EXPECT_NSEQ(data.expected_suggestions, GetSuggestionValues());
    EXPECT_NSEQ(data.expected_result,
                ExecuteJavaScript(kUsernamePasswordVerificationScript));
    // Clear all suggestions.
    [suggestionController_ setSuggestions:nil];
  }
}


// Check that if the PasswordController is told (by the PasswordManagerClient)
// that this is Incognito, it won't enable password generation.
TEST_F(PasswordControllerTest, IncognitoPasswordGenerationDisabled) {
    TearDown();
    ChromeWebTest::SetUp();

    PasswordFormManager::set_wait_for_server_predictions_for_filling(false);

    auto client =
    std::make_unique<NiceMock<MockPasswordManagerClient>>(store_.get());
    weak_client_ = client.get();

    EXPECT_CALL(*weak_client_->GetPasswordFeatureManager(),
                IsGenerationEnabled())
        .WillRepeatedly(Return(true));
    EXPECT_CALL(*weak_client_, IsIncognito()).WillRepeatedly(Return(true));

    passwordController_ =
    [[PasswordController alloc] initWithWebState:web_state()
                                          client:std::move(client)];

    EXPECT_FALSE([passwordController_ passwordGenerationHelper]);
}

// Tests that the user is prompted to save or update password on a succesful
// form submission.
TEST_F(PasswordControllerTest, ShowingSavingPromptOnSuccessfulSubmission) {
  const char* kHtml = {"<html><body>"
                       "<form name='login_form' id='login_form'>"
                       "  <input type='text' name='username'>"
                       "  <input type='password' name='password'>"
                       "  <button id='submit_button' value='Submit'>"
                       "</form>"
                       "</body></html>"};
  ON_CALL(*store_, GetLogins(_, _))
      .WillByDefault(WithArg<1>(InvokeEmptyConsumerWithForms()));

  LoadHtml(SysUTF8ToNSString(kHtml));

  std::unique_ptr<PasswordFormManagerForUI> form_manager_to_save;
  EXPECT_CALL(*weak_client_, PromptUserToSaveOrUpdatePasswordPtr(_))
      .WillOnce(WithArg<0>(SaveToScopedPtr(&form_manager_to_save)));
  ExecuteJavaScript(
      @"document.getElementsByName('username')[0].value = 'user1';"
       "document.getElementsByName('password')[0].value = 'password1';"
       "document.getElementById('submit_button').click();");
  LoadHtml(SysUTF8ToNSString("<html><body>Success</body></html>"));
  EXPECT_EQ("https://chromium.test/",
            form_manager_to_save->GetPendingCredentials().signon_realm);
  EXPECT_EQ(ASCIIToUTF16("user1"),
            form_manager_to_save->GetPendingCredentials().username_value);
  EXPECT_EQ(ASCIIToUTF16("password1"),
            form_manager_to_save->GetPendingCredentials().password_value);

  auto* form_manager =
      static_cast<PasswordFormManager*>(form_manager_to_save.get());
  EXPECT_TRUE(form_manager->is_submitted());
  EXPECT_FALSE(form_manager->IsPasswordUpdate());
}

// Tests that the user is not prompted to save or update password on
// leaving the page before submitting the form.
TEST_F(PasswordControllerTest, NotShowingSavingPromptWithoutSubmission) {
  const char* kHtml = {"<html><body>"
                       "<form name='login_form' id='login_form'>"
                       "  <input type='text' name='username'>"
                       "  <input type='password' name='password'>"
                       "  <button id='submit_button' value='Submit'>"
                       "</form>"
                       "</body></html>"};
  ON_CALL(*store_, GetLogins(_, _))
      .WillByDefault(WithArg<1>(InvokeEmptyConsumerWithForms()));

  LoadHtml(SysUTF8ToNSString(kHtml));

  EXPECT_CALL(*weak_client_, PromptUserToSaveOrUpdatePasswordPtr(_)).Times(0);
  ExecuteJavaScript(
      @"document.getElementsByName('username')[0].value = 'user1';"
       "document.getElementsByName('password')[0].value = 'password1';");
  LoadHtml(SysUTF8ToNSString("<html><body>New page</body></html>"));
}

// Tests that the user is not prompted to save or update password on a
// succesful form submission while saving is disabled.
TEST_F(PasswordControllerTest, NotShowingSavingPromptWhileSavingIsDisabled) {
  const char* kHtml = {"<html><body>"
                       "<form name='login_form' id='login_form'>"
                       "  <input type='text' name='username'>"
                       "  <input type='password' name='password'>"
                       "  <button id='submit_button' value='Submit'>"
                       "</form>"
                       "</body></html>"};
  ON_CALL(*store_, GetLogins(_, _))
      .WillByDefault(WithArg<1>(InvokeEmptyConsumerWithForms()));
  ON_CALL(*weak_client_, IsSavingAndFillingEnabled(_))
      .WillByDefault(Return(false));

  LoadHtml(SysUTF8ToNSString(kHtml));

  EXPECT_CALL(*weak_client_, PromptUserToSaveOrUpdatePasswordPtr(_)).Times(0);
  ExecuteJavaScript(
      @"document.getElementsByName('username')[0].value = 'user1';"
       "document.getElementsByName('password')[0].value = 'password1';"
       "document.getElementById('submit_button').click();");
  LoadHtml(SysUTF8ToNSString("<html><body>Success</body></html>"));
}

// Tests that the user is prompted to update password on a succesful
// form submission when there's already a credential with the same
// username in the store.
TEST_F(PasswordControllerTest, ShowingUpdatePromptOnSuccessfulSubmission) {
  PasswordForm form(MakeSimpleForm());
  ON_CALL(*store_, GetLogins(_, _))
      .WillByDefault(WithArg<1>(InvokeConsumer(form)));

  const char* kHtml = {"<html><body>"
                       "<form name='login_form' id='login_form'>"
                       "  <input type='text' name='Username'>"
                       "  <input type='password' name='Passwd'>"
                       "  <button id='submit_button' value='Submit'>"
                       "</form>"
                       "</body></html>"};

  LoadHtml(SysUTF8ToNSString(kHtml), GURL("http://www.google.com/a/LoginAuth"));

  std::unique_ptr<PasswordFormManagerForUI> form_manager_to_save;
  EXPECT_CALL(*weak_client_, PromptUserToSaveOrUpdatePasswordPtr(_))
      .WillOnce(WithArg<0>(SaveToScopedPtr(&form_manager_to_save)));
  ExecuteJavaScript(
      @"document.getElementsByName('Username')[0].value = 'googleuser';"
       "document.getElementsByName('Passwd')[0].value = 'new_password';"
       "document.getElementById('submit_button').click();");
  LoadHtml(SysUTF8ToNSString("<html><body>Success</body></html>"),
           GURL("http://www.google.com/a/Login"));
  EXPECT_EQ("http://www.google.com/",
            form_manager_to_save->GetPendingCredentials().signon_realm);
  EXPECT_EQ(ASCIIToUTF16("googleuser"),
            form_manager_to_save->GetPendingCredentials().username_value);
  EXPECT_EQ(ASCIIToUTF16("new_password"),
            form_manager_to_save->GetPendingCredentials().password_value);

  auto* form_manager =
      static_cast<PasswordFormManager*>(form_manager_to_save.get());
  EXPECT_TRUE(form_manager->is_submitted());
  EXPECT_TRUE(form_manager->IsPasswordUpdate());
}
