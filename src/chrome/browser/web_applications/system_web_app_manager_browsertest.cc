// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/web_applications/system_web_app_manager_browsertest.h"

#include <string>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/files/file_util.h"
#include "base/run_loop.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/apps/app_service/app_launch_params.h"
#include "chrome/browser/apps/app_service/app_service_proxy.h"
#include "chrome/browser/apps/app_service/app_service_proxy_factory.h"
#include "chrome/browser/apps/launch_service/launch_service.h"
#include "chrome/browser/native_file_system/native_file_system_permission_request_manager.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/web_applications/components/web_app_constants.h"
#include "chrome/browser/web_applications/components/web_app_helpers.h"
#include "chrome/browser/web_applications/test/test_system_web_app_installation.h"
#include "chrome/browser/web_applications/test/test_web_app_provider.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/webui_url_constants.h"
#include "chrome/services/app_service/public/cpp/app_registry_cache.h"
#include "chrome/services/app_service/public/cpp/app_update.h"
#include "components/permissions/permission_util.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_controller_factory.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/test_navigation_observer.h"
#include "content/public/test/test_utils.h"
#include "extensions/browser/extension_registry.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/features.h"
#include "url/gurl.h"

namespace web_app {

SystemWebAppManagerBrowserTest::SystemWebAppManagerBrowserTest(
    bool install_mock) {
  scoped_feature_list_.InitWithFeatures(
      {features::kSystemWebApps, blink::features::kNativeFileSystemAPI,
       blink::features::kFileHandlingAPI},
      {});
  if (install_mock) {
    maybe_installation_ =
        TestSystemWebAppInstallation::SetUpStandaloneSingleWindowApp();
  }
}

SystemWebAppManagerBrowserTest::~SystemWebAppManagerBrowserTest() = default;

SystemWebAppManager& SystemWebAppManagerBrowserTest::GetManager() {
  return WebAppProvider::Get(browser()->profile())->system_web_app_manager();
}

SystemAppType SystemWebAppManagerBrowserTest::GetMockAppType() {
  DCHECK(maybe_installation_);
  return maybe_installation_->GetType();
}

void SystemWebAppManagerBrowserTest::WaitForTestSystemAppInstall() {
  // Wait for the System Web Apps to install.
  if (maybe_installation_) {
    maybe_installation_->WaitForAppInstall();
  } else {
    GetManager().InstallSystemAppsForTesting();
  }
}

Browser* SystemWebAppManagerBrowserTest::WaitForSystemAppInstallAndLaunch(
    SystemAppType system_app_type) {
  WaitForTestSystemAppInstall();
  apps::AppLaunchParams params = LaunchParamsForApp(system_app_type);
  content::WebContents* web_contents = LaunchApp(params);
  Browser* browser = chrome::FindBrowserWithWebContents(web_contents);
  EXPECT_EQ(web_app::GetAppIdFromApplicationName(browser->app_name()),
            params.app_id);
  return browser;
}

apps::AppLaunchParams SystemWebAppManagerBrowserTest::LaunchParamsForApp(
    SystemAppType system_app_type) {
  base::Optional<AppId> app_id =
      GetManager().GetAppIdForSystemApp(system_app_type);
  DCHECK(app_id.has_value());
  return apps::AppLaunchParams(
      *app_id, apps::mojom::LaunchContainer::kLaunchContainerWindow,
      WindowOpenDisposition::CURRENT_TAB,
      apps::mojom::AppLaunchSource::kSourceTest);
}

content::WebContents* SystemWebAppManagerBrowserTest::LaunchApp(
    const apps::AppLaunchParams& params) {
  // Use apps::LaunchService::OpenApplication() to get the most coverage. E.g.,
  // this is what is invoked by file_manager::file_tasks::ExecuteWebTask() on
  // ChromeOS.
  return apps::LaunchService::Get(browser()->profile())
      ->OpenApplication(params);
}

content::EvalJsResult EvalJs(content::WebContents* web_contents,
                             const std::string& script) {
  // Set world_id = 1 to bypass Content Security Policy restriction.
  return content::EvalJs(web_contents, script,
                         content::EXECUTE_SCRIPT_DEFAULT_OPTIONS,
                         1 /*world_id*/);
}

::testing::AssertionResult ExecJs(content::WebContents* web_contents,
                                  const std::string& script) {
  // Set world_id = 1 to bypass Content Security Policy restriction.
  return content::ExecJs(web_contents, script,
                         content::EXECUTE_SCRIPT_DEFAULT_OPTIONS,
                         1 /*world_id*/);
}

// Test that System Apps install correctly with a manifest.
IN_PROC_BROWSER_TEST_F(SystemWebAppManagerBrowserTest, Install) {
  Browser* app_browser = WaitForSystemAppInstallAndLaunch(GetMockAppType());

  AppId app_id = app_browser->app_controller()->GetAppId();
  EXPECT_EQ(GetManager().GetAppIdForSystemApp(GetMockAppType()), app_id);
  EXPECT_TRUE(GetManager().IsSystemWebApp(app_id));

  Profile* profile = app_browser->profile();
  AppRegistrar& registrar =
      WebAppProviderBase::GetProviderBase(profile)->registrar();

  EXPECT_EQ("Test System App", registrar.GetAppShortName(app_id));
  EXPECT_EQ(SkColorSetRGB(0, 0xFF, 0), registrar.GetAppThemeColor(app_id));
  EXPECT_TRUE(registrar.HasExternalAppWithInstallSource(
      app_id, web_app::ExternalInstallSource::kSystemInstalled));
  EXPECT_EQ(
      registrar.FindAppWithUrlInScope(content::GetWebUIURL("test-system-app/")),
      app_id);

  if (!base::FeatureList::IsEnabled(features::kDesktopPWAsWithoutExtensions)) {
    const extensions::Extension* extension =
        extensions::ExtensionRegistry::Get(profile)->GetInstalledExtension(
            app_id);
    EXPECT_TRUE(extension->from_bookmark());
    EXPECT_EQ(extensions::Manifest::EXTERNAL_COMPONENT, extension->location());
  }

  apps::AppServiceProxy* proxy =
      apps::AppServiceProxyFactory::GetForProfile(browser()->profile());
  proxy->AppRegistryCache().ForOneApp(
      app_id, [](const apps::AppUpdate& update) {
        EXPECT_EQ(apps::mojom::OptionalBool::kTrue, update.ShowInLauncher());
      });
}

// Check the toolbar is not shown for system web apps for pages on the chrome://
// scheme but is shown off the chrome:// scheme.
IN_PROC_BROWSER_TEST_F(SystemWebAppManagerBrowserTest,
                       ToolbarVisibilityForSystemWebApp) {
  Browser* app_browser = WaitForSystemAppInstallAndLaunch(GetMockAppType());
  // In scope, the toolbar should not be visible.
  EXPECT_FALSE(app_browser->app_controller()->ShouldShowCustomTabBar());

  // Because the first part of the url is on a different origin (settings vs.
  // foo) a toolbar would normally be shown. However, because settings is a
  // SystemWebApp and foo is served via chrome:// it is okay not to show the
  // toolbar.
  GURL out_of_scope_chrome_page(content::kChromeUIScheme +
                                std::string("://foo"));
  content::NavigateToURLBlockUntilNavigationsComplete(
      app_browser->tab_strip_model()->GetActiveWebContents(),
      out_of_scope_chrome_page, 1);
  EXPECT_FALSE(app_browser->app_controller()->ShouldShowCustomTabBar());

  // Even though the url is secure it is not being served over chrome:// so a
  // toolbar should be shown.
  GURL off_scheme_page("https://example.com");
  content::NavigateToURLBlockUntilNavigationsComplete(
      app_browser->tab_strip_model()->GetActiveWebContents(), off_scheme_page,
      1);
  EXPECT_TRUE(app_browser->app_controller()->ShouldShowCustomTabBar());
}

// Check launch files are passed to application.
// Note: This test uses ExecuteScriptXXX instead of ExecJs and EvalJs because of
// some quirks surrounding origin trials and content security policies.
IN_PROC_BROWSER_TEST_F(SystemWebAppManagerBrowserTest,
                       LaunchFilesForSystemWebApp) {
  WaitForTestSystemAppInstall();
  apps::AppLaunchParams params = LaunchParamsForApp(GetMockAppType());
  params.source = apps::mojom::AppLaunchSource::kSourceChromeInternal;

  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedTempDir temp_directory;
  ASSERT_TRUE(temp_directory.CreateUniqueTempDir());
  base::FilePath temp_file_path;
  ASSERT_TRUE(base::CreateTemporaryFileInDir(temp_directory.GetPath(),
                                             &temp_file_path));

  const GURL& launch_url = WebAppProvider::Get(browser()->profile())
                               ->registrar()
                               .GetAppLaunchURL(params.app_id);

  // First launch.
  params.launch_files = {temp_file_path};
  content::TestNavigationObserver navigation_observer(launch_url);
  navigation_observer.StartWatchingNewWebContents();
  content::WebContents* web_contents =
      apps::LaunchService::Get(browser()->profile())->OpenApplication(params);
  navigation_observer.Wait();

  // Set up a Promise that resolves to launchParams, when launchQueue's consumer
  // callback is called.
  EXPECT_TRUE(content::ExecuteScript(
      web_contents,
      "window.launchParamsPromise = new Promise(resolve => {"
      "  window.resolveLaunchParamsPromise = resolve;"
      "});"
      "launchQueue.setConsumer(launchParams => {"
      "  window.resolveLaunchParamsPromise(launchParams);"
      "});"));

  // Check launch files are correct.
  std::string file_name;
  EXPECT_TRUE(content::ExecuteScriptAndExtractString(
      web_contents,
      "window.launchParamsPromise.then("
      "  launchParams => "
      "    domAutomationController.send(launchParams.files[0].name));",
      &file_name));
  EXPECT_EQ(temp_file_path.BaseName().AsUTF8Unsafe(), file_name);

  // Reset the Promise to get second launchParams.
  EXPECT_TRUE(content::ExecuteScript(
      web_contents,
      "window.launchParamsPromise = new Promise(resolve => {"
      "  window.resolveLaunchParamsPromise = resolve;"
      "});"));

  // Second Launch.
  base::FilePath temp_file_path2;
  ASSERT_TRUE(base::CreateTemporaryFileInDir(temp_directory.GetPath(),
                                             &temp_file_path2));
  params.launch_files = {temp_file_path2};
  content::WebContents* web_contents2 =
      apps::LaunchService::Get(browser()->profile())->OpenApplication(params);

  // WebContents* should be the same because we are passing launchParams to the
  // opened application.
  EXPECT_EQ(web_contents, web_contents2);

  // Second launch_files are passed to the opened application.
  EXPECT_TRUE(content::ExecuteScriptAndExtractString(
      web_contents,
      "window.launchParamsPromise.then("
      "  launchParams => "
      "    domAutomationController.send(launchParams.files[0].name))",
      &file_name));
  EXPECT_EQ(temp_file_path2.BaseName().AsUTF8Unsafe(), file_name);
}

class SystemWebAppManagerLaunchFilesBrowserTest
    : public SystemWebAppManagerBrowserTest,
      public testing::WithParamInterface<std::vector<base::Feature>> {
 public:
  SystemWebAppManagerLaunchFilesBrowserTest()
      : SystemWebAppManagerBrowserTest(/*install_mock=*/false) {
    scoped_feature_list_.InitWithFeatures(GetParam(), {});
    maybe_installation_ =
        TestSystemWebAppInstallation::SetUpAppThatReceivesLaunchDirectory();
  }

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
};

// Launching behavior for apps that do not want to received launch directory are
// tested in |SystemWebAppManagerBrowserTest.LaunchFilesForSystemWebApp|.
// Note: This test uses ExecuteScriptXXX instead of ExecJs and EvalJs because of
// some quirks surrounding origin trials and content security policies.
IN_PROC_BROWSER_TEST_P(SystemWebAppManagerLaunchFilesBrowserTest,
                       LaunchDirectoryForSystemWebApp) {
  WaitForTestSystemAppInstall();
  apps::AppLaunchParams params = LaunchParamsForApp(GetMockAppType());
  params.source = apps::mojom::AppLaunchSource::kSourceChromeInternal;

  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedTempDir temp_directory;
  ASSERT_TRUE(temp_directory.CreateUniqueTempDir());
  base::FilePath temp_file_path;
  ASSERT_TRUE(base::CreateTemporaryFileInDir(temp_directory.GetPath(),
                                             &temp_file_path));

  const GURL& launch_url = WebAppProvider::Get(browser()->profile())
                               ->registrar()
                               .GetAppLaunchURL(params.app_id);

  // First launch.
  params.launch_files = {temp_file_path};
  content::TestNavigationObserver navigation_observer(launch_url);
  navigation_observer.StartWatchingNewWebContents();
  content::WebContents* web_contents =
      apps::LaunchService::Get(browser()->profile())->OpenApplication(params);
  navigation_observer.Wait();

  // Set up a Promise that resolves to launchParams, when launchQueue's consumer
  // callback is called.
  EXPECT_TRUE(content::ExecuteScript(
      web_contents,
      "window.launchParamsPromise = new Promise(resolve => {"
      "  window.resolveLaunchParamsPromise = resolve;"
      "});"
      "launchQueue.setConsumer(launchParams => {"
      "  window.resolveLaunchParamsPromise(launchParams);"
      "});"));

  // Wait for launch. Set window.firstLaunchParams for inspection.
  EXPECT_TRUE(
      content::ExecuteScript(web_contents,
                             "window.launchParamsPromise.then(launchParams => {"
                             "  window.firstLaunchParams = launchParams;"
                             "});"));

  // Check launch directory is correct.
  bool is_directory;
  EXPECT_TRUE(content::ExecuteScriptAndExtractBool(
      web_contents,
      "domAutomationController.send(window.firstLaunchParams."
      "files[0].isDirectory)",
      &is_directory));
  EXPECT_TRUE(is_directory);

  std::string file_name;
  EXPECT_TRUE(content::ExecuteScriptAndExtractString(
      web_contents,
      "domAutomationController.send(window.firstLaunchParams.files[0].name)",
      &file_name));
  EXPECT_EQ(temp_directory.GetPath().BaseName().AsUTF8Unsafe(), file_name);

  // Check launch files are correct.
  bool is_file;
  EXPECT_TRUE(content::ExecuteScriptAndExtractBool(
      web_contents,
      "domAutomationController.send(window.firstLaunchParams.files[1].isFile)",
      &is_file));
  EXPECT_TRUE(content::ExecuteScriptAndExtractString(
      web_contents,
      "domAutomationController.send(window.firstLaunchParams.files[1].name)",
      &file_name));
  EXPECT_EQ(temp_file_path.BaseName().AsUTF8Unsafe(), file_name);

  // Reset the Promise to get second launchParams.
  EXPECT_TRUE(content::ExecuteScript(
      web_contents,
      "window.launchParamsPromise = new Promise(resolve => {"
      "  window.resolveLaunchParamsPromise = resolve;"
      "});"));

  // Second Launch.
  base::ScopedTempDir temp_directory2;
  ASSERT_TRUE(temp_directory2.CreateUniqueTempDir());
  base::FilePath temp_file_path2;
  ASSERT_TRUE(base::CreateTemporaryFileInDir(temp_directory2.GetPath(),
                                             &temp_file_path2));
  params.launch_files = {temp_file_path2};
  content::WebContents* web_contents2 =
      apps::LaunchService::Get(browser()->profile())->OpenApplication(params);

  // WebContents* should be the same because we are passing launchParams to the
  // opened application.
  EXPECT_EQ(web_contents, web_contents2);

  // Wait for launch. Sets window.secondLaunchParams for inspection.
  EXPECT_TRUE(
      content::ExecuteScript(web_contents,
                             "window.launchParamsPromise.then(launchParams => {"
                             "  window.secondLaunchParams = launchParams;"
                             "});"));

  // Second launch_dir and launch_files are passed to the opened application.
  EXPECT_TRUE(content::ExecuteScriptAndExtractBool(
      web_contents,
      "domAutomationController.send(window.secondLaunchParams.files[0]."
      "isDirectory)",
      &is_directory));
  EXPECT_TRUE(is_directory);

  EXPECT_TRUE(content::ExecuteScriptAndExtractString(
      web_contents,
      "domAutomationController.send(window.secondLaunchParams.files[0].name)",
      &file_name));
  EXPECT_EQ(temp_directory2.GetPath().BaseName().AsUTF8Unsafe(), file_name);

  EXPECT_TRUE(content::ExecuteScriptAndExtractBool(
      web_contents,
      "domAutomationController.send(window.secondLaunchParams.files[1]."
      "isFile)",
      &is_file));
  EXPECT_TRUE(is_file);

  EXPECT_TRUE(content::ExecuteScriptAndExtractString(
      web_contents,
      "domAutomationController.send(window.secondLaunchParams.files[1].name)",
      &file_name));
  EXPECT_EQ(temp_file_path2.BaseName().AsUTF8Unsafe(), file_name);

  // Launch directories and files passed to system web apps should automatically
  // be granted write permission. Users should not get permission prompts. Here
  // we execute some JavaScript code that modifies and deletes files in the
  // directory.

  // Auto deny prompts (if they show up).
  NativeFileSystemPermissionRequestManager::FromWebContents(web_contents)
      ->set_auto_response_for_test(permissions::PermissionAction::DENIED);

  // Modifies the launch file. Reuse the first launch directory.
  bool writer_closed;
  EXPECT_TRUE(content::ExecuteScriptAndExtractBool(
      web_contents,
      "window.firstLaunchParams.files[1].createWriter().then("
      "  async writer => {"
      "    console.log(writer);"
      "    await writer.write(0, 'test');"
      "    await writer.close();"
      "    domAutomationController.send(true);"
      "  }"
      ");",
      &writer_closed));
  EXPECT_TRUE(writer_closed);

  std::string read_contents;
  EXPECT_TRUE(base::ReadFileToString(temp_file_path, &read_contents));
  EXPECT_EQ("test", read_contents);

  // Deletes the launch file. Reuse the second launch directory.
  bool file_removed;
  EXPECT_TRUE(content::ExecuteScriptAndExtractBool(
      web_contents,
      "window.secondLaunchParams.files[0].removeEntry("
      "  window.secondLaunchParams.files[1].name"
      ").then(_ => domAutomationController.send(true));",
      &file_removed));
  EXPECT_TRUE(file_removed);
  EXPECT_FALSE(base::PathExists(temp_file_path2));
}

class SystemWebAppManagerNotShownInLauncherTest
    : public SystemWebAppManagerBrowserTest {
 public:
  SystemWebAppManagerNotShownInLauncherTest()
      : SystemWebAppManagerBrowserTest(/*install_mock=*/false) {
    maybe_installation_ =
        TestSystemWebAppInstallation::SetUpAppNotShownInLauncher();
  }
};

IN_PROC_BROWSER_TEST_F(SystemWebAppManagerNotShownInLauncherTest,
                       NotShownInLauncher) {
  WaitForSystemAppInstallAndLaunch(GetMockAppType());
  AppId app_id = GetManager().GetAppIdForSystemApp(GetMockAppType()).value();

  apps::AppServiceProxy* proxy =
      apps::AppServiceProxyFactory::GetForProfile(browser()->profile());
  proxy->AppRegistryCache().ForOneApp(
      app_id, [](const apps::AppUpdate& update) {
        EXPECT_EQ(apps::mojom::OptionalBool::kFalse, update.ShowInLauncher());
      });
}

class SystemWebAppManagerAdditionalSearchTermsTest
    : public SystemWebAppManagerBrowserTest {
 public:
  SystemWebAppManagerAdditionalSearchTermsTest()
      : SystemWebAppManagerBrowserTest(/*install_mock=*/false) {
    maybe_installation_ =
        TestSystemWebAppInstallation::SetUpAppWithAdditionalSearchTerms();
  }
};

IN_PROC_BROWSER_TEST_F(SystemWebAppManagerAdditionalSearchTermsTest,
                       AdditionalSearchTerms) {
  WaitForSystemAppInstallAndLaunch(GetMockAppType());
  AppId app_id = GetManager().GetAppIdForSystemApp(GetMockAppType()).value();

  apps::AppServiceProxy* proxy =
      apps::AppServiceProxyFactory::GetForProfile(browser()->profile());
  proxy->AppRegistryCache().ForOneApp(
      app_id, [](const apps::AppUpdate& update) {
        EXPECT_EQ(std::vector<std::string>({"Security"}),
                  update.AdditionalSearchTerms());
      });
}

INSTANTIATE_TEST_SUITE_P(
    PermissionContext,
    SystemWebAppManagerLaunchFilesBrowserTest,
    testing::Values(
        /*default_enabled_permission_context*/ std::vector<base::Feature>(),
        /*origin_scoped_permission_context*/ std::vector<base::Feature>(
            {features::kNativeFileSystemOriginScopedPermissions})));

}  // namespace web_app
