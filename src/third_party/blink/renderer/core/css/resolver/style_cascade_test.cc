// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/resolver/style_cascade.h"

#include <vector>

#include "third_party/blink/renderer/bindings/core/v8/v8_css_style_sheet_init.h"
#include "third_party/blink/renderer/core/animation/css/css_animations.h"
#include "third_party/blink/renderer/core/css/active_style_sheets.h"
#include "third_party/blink/renderer/core/css/css_custom_property_declaration.h"
#include "third_party/blink/renderer/core/css/css_pending_substitution_value.h"
#include "third_party/blink/renderer/core/css/css_primitive_value.h"
#include "third_party/blink/renderer/core/css/css_test_helpers.h"
#include "third_party/blink/renderer/core/css/css_variable_reference_value.h"
#include "third_party/blink/renderer/core/css/document_style_environment_variables.h"
#include "third_party/blink/renderer/core/css/media_query_evaluator.h"
#include "third_party/blink/renderer/core/css/parser/css_parser_context.h"
#include "third_party/blink/renderer/core/css/parser/css_parser_local_context.h"
#include "third_party/blink/renderer/core/css/parser/css_parser_token_range.h"
#include "third_party/blink/renderer/core/css/parser/css_property_parser.h"
#include "third_party/blink/renderer/core/css/parser/css_tokenizer.h"
#include "third_party/blink/renderer/core/css/parser/css_variable_parser.h"
#include "third_party/blink/renderer/core/css/properties/css_property_instances.h"
#include "third_party/blink/renderer/core/css/properties/css_property_ref.h"
#include "third_party/blink/renderer/core/css/properties/longhands/custom_property.h"
#include "third_party/blink/renderer/core/css/property_registry.h"
#include "third_party/blink/renderer/core/css/resolver/cascade_filter.h"
#include "third_party/blink/renderer/core/css/resolver/cascade_interpolations.h"
#include "third_party/blink/renderer/core/css/resolver/cascade_map.h"
#include "third_party/blink/renderer/core/css/resolver/cascade_priority.h"
#include "third_party/blink/renderer/core/css/resolver/scoped_style_resolver.h"
#include "third_party/blink/renderer/core/css/resolver/style_resolver.h"
#include "third_party/blink/renderer/core/css/style_cascade_slots.h"
#include "third_party/blink/renderer/core/css/style_engine.h"
#include "third_party/blink/renderer/core/css/style_sheet_contents.h"
#include "third_party/blink/renderer/core/html/html_element.h"
#include "third_party/blink/renderer/core/style/computed_style.h"
#include "third_party/blink/renderer/core/style_property_shorthand.h"
#include "third_party/blink/renderer/core/testing/page_test_base.h"
#include "third_party/blink/renderer/platform/testing/runtime_enabled_features_test_helpers.h"
#include "third_party/blink/renderer/platform/wtf/text/string_builder.h"

namespace blink {

using css_test_helpers::ParseDeclarationBlock;
using css_test_helpers::RegisterProperty;
using Origin = CascadeOrigin;
using Priority = CascadePriority;
using UnitType = CSSPrimitiveValue::UnitType;

class TestCascade {
  STACK_ALLOCATED();

 public:
  TestCascade(Document& document, Element* target = nullptr)
      : state_(document, target ? *target : *document.body()),
        cascade_(InitState(state_)) {}

  scoped_refptr<ComputedStyle> TakeStyle() { return state_.TakeStyle(); }

  StyleResolverState& State() { return state_; }
  StyleCascade& InnerCascade() { return cascade_; }

  void InheritFrom(scoped_refptr<ComputedStyle> parent) {
    state_.SetParentStyle(parent);
    state_.StyleRef().InheritFrom(*parent);
  }

  // TestCascade has two main APIs:

  // 1. Direct Analyze & Apply. This is a simple wrapper for Analyze & Apply
  //    on the inner cascade.

  void Analyze(const MatchResult& result,
               CascadeFilter filter = CascadeFilter()) {
    cascade_.Analyze(result, filter);
  }

  void Apply(const MatchResult& result,
             CascadeFilter filter = CascadeFilter()) {
    cascade_.Apply(result, filter);
  }

  // 2. "Add" API. This allows the caller to build a MatchResult from strings.
  //
  //   TestCascade cascade(GetDocument());
  //   cascade.Add("color:green;top:1px", CascadeOrigin::kUserAgent);
  //   cascade.Add("color:red");
  //   cascade.Apply();
  //
  //  The Add() functions will parse the declaration blocks and add the result
  //  to an internal MatchResult object. The param-less Apply() function will
  //  then Analyze that MatchResult and immediately Apply it.
  //
  //  Note that because of how MatchResult works, declarations must be added
  //  in "origin order", i.e. UserAgent first, then User, then Author.

  void Add(String block, CascadeOrigin origin = CascadeOrigin::kAuthor) {
    CSSParserMode mode =
        origin == CascadeOrigin::kUserAgent ? kUASheetMode : kHTMLStandardMode;
    Add(ParseDeclarationBlock(block, mode), origin);
  }

  void Add(String name, String value, CascadeOrigin origin = Origin::kAuthor) {
    Add(name + ":" + value, origin);
  }

  void Add(const CSSPropertyValueSet* set,
           CascadeOrigin origin = CascadeOrigin::kAuthor) {
    DCHECK_LE(origin, CascadeOrigin::kAuthor) << "Animations not supported";
    DCHECK_LE(current_origin_, origin) << "Please add declarations in order";
    EnsureAtLeast(origin);
    match_result_.AddMatchedProperties(set);
  }

  void Apply(CascadeFilter filter = CascadeFilter()) {
    EnsureAtLeast(CascadeOrigin::kAuthor);
    cascade_.Analyze(match_result_, filter);
    auto interpolations = GetInterpolations();
    cascade_.Apply(&match_result_,
                   interpolations.IsEmpty() ? nullptr : &interpolations,
                   filter);
  }

  String ComputedValue(String name) const {
    CSSPropertyRef ref(name, GetDocument());
    DCHECK(ref.IsValid());
    const LayoutObject* layout_object = nullptr;
    bool allow_visited_style = false;
    const CSSValue* value = ref.GetProperty().CSSValueFromComputedStyle(
        *state_.Style(), layout_object, allow_visited_style);
    return value ? value->CssText() : g_null_atom;
  }

  CascadePriority GetPriority(String name) {
    return GetPriority(
        *CSSPropertyName::From(GetDocument().ToExecutionContext(), name));
  }

  CascadePriority GetPriority(CSSPropertyName name) {
    CascadePriority* c = cascade_.map_.Find(name);
    return c ? *c : CascadePriority();
  }

  CascadeOrigin GetOrigin(String name) { return GetPriority(name).GetOrigin(); }

  void CalculateTransitionUpdate() {
    CSSAnimations::CalculateTransitionUpdate(
        state_.AnimationUpdate(), CSSAnimations::PropertyPass::kCustom,
        &state_.GetElement(), *state_.Style());
    CSSAnimations::CalculateTransitionUpdate(
        state_.AnimationUpdate(), CSSAnimations::PropertyPass::kStandard,
        &state_.GetElement(), *state_.Style());
  }

  void CalculateAnimationUpdate() {
    CSSAnimations::CalculateAnimationUpdate(
        state_.AnimationUpdate(), &state_.GetElement(), state_.GetElement(),
        *state_.Style(), state_.ParentStyle(),
        &GetDocument().EnsureStyleResolver());
  }

  void AnalyzeAnimations() {
    CalculateAnimationUpdate();
    cascade_.Analyze(GetInterpolations(), CascadeFilter());
  }
  void AnalyzeTransitions() {
    CalculateTransitionUpdate();
    cascade_.Analyze(GetInterpolations(), CascadeFilter());
  }

 private:
  Document& GetDocument() const { return state_.GetDocument(); }
  Element* Body() const { return GetDocument().body(); }

  static StyleResolverState& InitState(StyleResolverState& state) {
    state.SetStyle(InitialStyle(state.GetDocument()));
    state.SetParentStyle(InitialStyle(state.GetDocument()));
    return state;
  }

  static scoped_refptr<ComputedStyle> InitialStyle(Document& document) {
    return StyleResolver::InitialStyleForElement(document);
  }

  void FinishOrigin() {
    switch (current_origin_) {
      case CascadeOrigin::kUserAgent:
        match_result_.FinishAddingUARules();
        current_origin_ = CascadeOrigin::kUser;
        break;
      case CascadeOrigin::kUser:
        match_result_.FinishAddingUserRules();
        current_origin_ = CascadeOrigin::kAuthor;
        break;
      case CascadeOrigin::kAuthor:
      default:
        NOTREACHED();
        break;
    }
  }

  void EnsureAtLeast(CascadeOrigin origin) {
    while (current_origin_ < origin)
      FinishOrigin();
  }

  CascadeInterpolations GetInterpolations() {
    const auto& update = state_.AnimationUpdate();
    if (update.IsEmpty())
      return CascadeInterpolations();
    using Entry = CascadeInterpolations::Entry;
    return CascadeInterpolations(Vector<Entry, 4>({
        Entry{&update.ActiveInterpolationsForCustomAnimations(),
              CascadeOrigin::kAnimation},
        Entry{&update.ActiveInterpolationsForStandardAnimations(),
              CascadeOrigin::kAnimation},
        Entry{&update.ActiveInterpolationsForCustomTransitions(),
              CascadeOrigin::kTransition},
        Entry{&update.ActiveInterpolationsForStandardTransitions(),
              CascadeOrigin::kTransition},
    }));
  }

  CascadeOrigin current_origin_ = CascadeOrigin::kUserAgent;
  MatchResult match_result_;
  StyleResolverState state_;
  StyleCascade cascade_;
};

class TestCascadeResolver {
  STACK_ALLOCATED();

 public:
  explicit TestCascadeResolver(Document& document)
      : document_(document), resolver_(CascadeFilter(), nullptr, nullptr, 0) {}
  bool InCycle() const { return resolver_.InCycle(); }
  bool DetectCycle(String name) {
    CSSPropertyRef ref(name, document_);
    DCHECK(ref.IsValid());
    const CSSProperty& property = ref.GetProperty();
    return resolver_.DetectCycle(property);
  }
  wtf_size_t CycleDepth() const { return resolver_.cycle_depth_; }

 private:
  friend class TestCascadeAutoLock;

  Document& document_;
  StyleCascade::Resolver resolver_;
};

class TestCascadeAutoLock {
  STACK_ALLOCATED();

 public:
  TestCascadeAutoLock(const CSSPropertyName& name,
                      TestCascadeResolver& resolver)
      : lock_(name, resolver.resolver_) {}

 private:
  StyleCascade::AutoLock lock_;
};

class StyleCascadeTest : public PageTestBase, private ScopedCSSCascadeForTest {
 public:
  StyleCascadeTest() : ScopedCSSCascadeForTest(true) {}

  CSSStyleSheet* CreateSheet(const String& css_text) {
    auto* init = MakeGarbageCollected<CSSStyleSheetInit>();
    DummyExceptionStateForTesting exception_state;
    CSSStyleSheet* sheet =
        CSSStyleSheet::Create(GetDocument(), init, exception_state);
    sheet->replaceSync(css_text, exception_state);
    sheet->Contents()->EnsureRuleSet(MediaQueryEvaluator(),
                                     kRuleHasNoSpecialState);
    return sheet;
  }

  void AppendSheet(const String& css_text) {
    CSSStyleSheet* sheet = CreateSheet(css_text);
    ASSERT_TRUE(sheet);

    Element* body = GetDocument().body();
    ASSERT_TRUE(body->IsInTreeScope());
    TreeScope& tree_scope = body->GetTreeScope();
    ScopedStyleResolver& scoped_resolver =
        tree_scope.EnsureScopedStyleResolver();
    ActiveStyleSheetVector active_sheets;
    active_sheets.push_back(
        std::make_pair(sheet, &sheet->Contents()->GetRuleSet()));
    scoped_resolver.AppendActiveStyleSheets(0, active_sheets);
  }

  Element* DocumentElement() const { return GetDocument().documentElement(); }

  void SetRootFont(String value) {
    DocumentElement()->SetInlineStyleProperty(CSSPropertyID::kFontSize, value);
    UpdateAllLifecyclePhasesForTest();
  }

  const MutableCSSPropertyValueSet* AnimationTaintedSet(AtomicString name,
                                                        String value) {
    CSSParserMode mode = kHTMLStandardMode;
    auto* set = MakeGarbageCollected<MutableCSSPropertyValueSet>(mode);
    set->SetProperty(name, value, /* important */ false,
                     SecureContextMode::kSecureContext,
                     /* context_style_sheet */ nullptr,
                     /* is_animation_tainted */ true);
    return set;
  }

  // Temporarily create a CSS Environment Variable.
  // https://drafts.csswg.org/css-env-1/
  class AutoEnv {
    STACK_ALLOCATED();

   public:
    AutoEnv(PageTestBase& test, AtomicString name, String value)
        : document_(&test.GetDocument()), name_(name) {
      EnsureEnvironmentVariables().SetVariable(name, value);
    }
    ~AutoEnv() { EnsureEnvironmentVariables().RemoveVariable(name_); }

   private:
    DocumentStyleEnvironmentVariables& EnsureEnvironmentVariables() {
      return document_->GetStyleEngine().EnsureEnvironmentVariables();
    }

    Document* document_;
    AtomicString name_;
  };
};

TEST_F(StyleCascadeTest, ApplySingle) {
  TestCascade cascade(GetDocument());
  cascade.Add("width", "1px", CascadeOrigin::kUserAgent);
  cascade.Add("width", "2px", CascadeOrigin::kAuthor);
  cascade.Apply();

  EXPECT_EQ("2px", cascade.ComputedValue("width"));
}

TEST_F(StyleCascadeTest, ApplyImportance) {
  TestCascade cascade(GetDocument());
  cascade.Add("width:1px !important", CascadeOrigin::kUserAgent);
  cascade.Add("width:2px", CascadeOrigin::kAuthor);
  cascade.Apply();

  EXPECT_EQ("1px", cascade.ComputedValue("width"));
}

TEST_F(StyleCascadeTest, ApplyAll) {
  TestCascade cascade(GetDocument());
  cascade.Add("width:1px", CascadeOrigin::kUserAgent);
  cascade.Add("height:1px", CascadeOrigin::kUserAgent);
  cascade.Add("all:initial", CascadeOrigin::kAuthor);
  cascade.Apply();

  EXPECT_EQ("auto", cascade.ComputedValue("width"));
  EXPECT_EQ("auto", cascade.ComputedValue("height"));
}

TEST_F(StyleCascadeTest, ApplyAllImportance) {
  TestCascade cascade(GetDocument());
  cascade.Add("opacity:0.5", CascadeOrigin::kUserAgent);
  cascade.Add("display:block !important", CascadeOrigin::kUserAgent);
  cascade.Add("all:initial", CascadeOrigin::kAuthor);
  cascade.Apply();

  EXPECT_EQ("1", cascade.ComputedValue("opacity"));
  EXPECT_EQ("block", cascade.ComputedValue("display"));
}

TEST_F(StyleCascadeTest, ApplyAllWithPhysicalLonghands) {
  TestCascade cascade(GetDocument());
  cascade.Add("width:1px", CascadeOrigin::kUserAgent);
  cascade.Add("height:1px !important", CascadeOrigin::kUserAgent);
  cascade.Add("all:initial", CascadeOrigin::kAuthor);
  cascade.Apply();
  EXPECT_EQ("auto", cascade.ComputedValue("width"));
  EXPECT_EQ("1px", cascade.ComputedValue("height"));
}

TEST_F(StyleCascadeTest, ApplyCustomProperty) {
  TestCascade cascade(GetDocument());
  cascade.Add("--x", " 10px ");
  cascade.Add("--y", "nope");
  cascade.Apply();

  EXPECT_EQ(" 10px ", cascade.ComputedValue("--x"));
  EXPECT_EQ("nope", cascade.ComputedValue("--y"));
}

TEST_F(StyleCascadeTest, ApplyGenerations) {
  TestCascade cascade(GetDocument());

  cascade.Add("--x:10px");
  cascade.Add("width:20px");
  cascade.Apply();
  EXPECT_EQ("10px", cascade.ComputedValue("--x"));
  EXPECT_EQ("20px", cascade.ComputedValue("width"));

  cascade.State().StyleRef().SetWidth(Length::Auto());
  cascade.State().StyleRef().SetVariableData("--x", nullptr, true);
  EXPECT_EQ(g_null_atom, cascade.ComputedValue("--x"));
  EXPECT_EQ("auto", cascade.ComputedValue("width"));

  // Apply again
  cascade.Apply();
  EXPECT_EQ("10px", cascade.ComputedValue("--x"));
  EXPECT_EQ("20px", cascade.ComputedValue("width"));
}

TEST_F(StyleCascadeTest, ApplyCustomPropertyVar) {
  // Apply --x first.
  {
    TestCascade cascade(GetDocument());
    cascade.Add("--x", "yes and var(--y)");
    cascade.Add("--y", "no");
    cascade.Apply();

    EXPECT_EQ("yes and no", cascade.ComputedValue("--x"));
    EXPECT_EQ("no", cascade.ComputedValue("--y"));
  }

  // Apply --y first.
  {
    TestCascade cascade(GetDocument());
    cascade.Add("--y", "no");
    cascade.Add("--x", "yes and var(--y)");
    cascade.Apply();

    EXPECT_EQ("yes and no", cascade.ComputedValue("--x"));
    EXPECT_EQ("no", cascade.ComputedValue("--y"));
  }
}

TEST_F(StyleCascadeTest, InvalidVarReferenceCauseInvalidVariable) {
  TestCascade cascade(GetDocument());
  cascade.Add("--x", "nope var(--y)");
  cascade.Apply();

  EXPECT_EQ(g_null_atom, cascade.ComputedValue("--x"));
}

TEST_F(StyleCascadeTest, ApplyCustomPropertyFallback) {
  TestCascade cascade(GetDocument());
  cascade.Add("--x", "yes and var(--y,no)");
  cascade.Apply();

  EXPECT_EQ("yes and no", cascade.ComputedValue("--x"));
}

TEST_F(StyleCascadeTest, RegisteredPropertyFallback) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);

  TestCascade cascade(GetDocument());
  cascade.Add("--x", "var(--y,10px)");
  cascade.Apply();

  EXPECT_EQ("10px", cascade.ComputedValue("--x"));
}

TEST_F(StyleCascadeTest, RegisteredPropertyFallbackValidation) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);

  TestCascade cascade(GetDocument());
  cascade.Add("--x", "10px");
  cascade.Add("--y", "var(--x,red)");  // Fallback must be valid <length>.
  cascade.Add("--z", "var(--y,pass)");
  cascade.Apply();

  EXPECT_EQ("pass", cascade.ComputedValue("--z"));
}

TEST_F(StyleCascadeTest, VarInFallback) {
  TestCascade cascade(GetDocument());
  cascade.Add("--x", "one var(--z,two var(--y))");
  cascade.Add("--y", "three");
  cascade.Apply();

  EXPECT_EQ("one two three", cascade.ComputedValue("--x"));
}

TEST_F(StyleCascadeTest, VarReferenceInNormalProperty) {
  TestCascade cascade(GetDocument());
  cascade.Add("--x", "10px");
  cascade.Add("width", "var(--x)");
  cascade.Apply();

  EXPECT_EQ("10px", cascade.ComputedValue("width"));
}

TEST_F(StyleCascadeTest, MultipleVarRefs) {
  TestCascade cascade(GetDocument());
  cascade.Add("--x", "var(--y) bar var(--y)");
  cascade.Add("--y", "foo");
  cascade.Apply();

  EXPECT_EQ("foo bar foo", cascade.ComputedValue("--x"));
}

TEST_F(StyleCascadeTest, RegisteredPropertyComputedValue) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);

  TestCascade cascade(GetDocument());
  cascade.Add("--x", "1in");
  cascade.Apply();

  EXPECT_EQ("96px", cascade.ComputedValue("--x"));
}

TEST_F(StyleCascadeTest, RegisteredPropertySyntaxErrorCausesInitial) {
  RegisterProperty(GetDocument(), "--x", "<length>", "10px", false);

  TestCascade cascade(GetDocument());
  cascade.Add("--x", "#fefefe");
  cascade.Add("--y", "var(--x)");
  cascade.Apply();

  EXPECT_EQ("10px", cascade.ComputedValue("--x"));
  EXPECT_EQ("10px", cascade.ComputedValue("--y"));
}

TEST_F(StyleCascadeTest, RegisteredPropertySubstitution) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);

  TestCascade cascade(GetDocument());
  cascade.Add("--x", "1in");
  cascade.Add("--y", "var(--x)");
  cascade.Apply();

  EXPECT_EQ("96px", cascade.ComputedValue("--y"));
}

TEST_F(StyleCascadeTest, RegisteredPropertyChain) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);
  RegisterProperty(GetDocument(), "--z", "<length>", "0px", false);

  TestCascade cascade(GetDocument());
  cascade.Add("--x", "1in");
  cascade.Add("--y", "var(--x)");
  cascade.Add("--z", "calc(var(--y) + 1in)");
  cascade.Apply();

  EXPECT_EQ("96px", cascade.ComputedValue("--x"));
  EXPECT_EQ("96px", cascade.ComputedValue("--y"));
  EXPECT_EQ("192px", cascade.ComputedValue("--z"));
}

TEST_F(StyleCascadeTest, BasicShorthand) {
  TestCascade cascade(GetDocument());
  cascade.Add("margin", "1px 2px 3px 4px");
  cascade.Apply();

  EXPECT_EQ("1px", cascade.ComputedValue("margin-top"));
  EXPECT_EQ("2px", cascade.ComputedValue("margin-right"));
  EXPECT_EQ("3px", cascade.ComputedValue("margin-bottom"));
  EXPECT_EQ("4px", cascade.ComputedValue("margin-left"));
}

TEST_F(StyleCascadeTest, BasicVarShorthand) {
  TestCascade cascade(GetDocument());
  cascade.Add("margin", "1px var(--x) 3px 4px");
  cascade.Add("--x", "2px");
  cascade.Apply();

  EXPECT_EQ("1px", cascade.ComputedValue("margin-top"));
  EXPECT_EQ("2px", cascade.ComputedValue("margin-right"));
  EXPECT_EQ("3px", cascade.ComputedValue("margin-bottom"));
  EXPECT_EQ("4px", cascade.ComputedValue("margin-left"));
}

TEST_F(StyleCascadeTest, ApplyingPendingSubstitutionFirst) {
  TestCascade cascade(GetDocument());
  cascade.Add("margin", "1px var(--x) 3px 4px");
  cascade.Add("--x", "2px");
  cascade.Add("margin-right", "5px");
  cascade.Apply();

  EXPECT_EQ("1px", cascade.ComputedValue("margin-top"));
  EXPECT_EQ("5px", cascade.ComputedValue("margin-right"));
  EXPECT_EQ("3px", cascade.ComputedValue("margin-bottom"));
  EXPECT_EQ("4px", cascade.ComputedValue("margin-left"));
}

TEST_F(StyleCascadeTest, ApplyingPendingSubstitutionLast) {
  TestCascade cascade(GetDocument());
  cascade.Add("margin-right", "5px");
  cascade.Add("margin", "1px var(--x) 3px 4px");
  cascade.Add("--x", "2px");
  cascade.Apply();

  EXPECT_EQ("1px", cascade.ComputedValue("margin-top"));
  EXPECT_EQ("2px", cascade.ComputedValue("margin-right"));
  EXPECT_EQ("3px", cascade.ComputedValue("margin-bottom"));
  EXPECT_EQ("4px", cascade.ComputedValue("margin-left"));
}

// TODO(andruud): This is not useful anymore.
// TEST_F(StyleCascadeTest, ApplyingPendingSubstitutionModifiesCascade) {
//  TestCascade cascade(GetDocument());
//  cascade.Add("margin", "1px var(--x) 3px 4px");
//  cascade.Add("--x", "2px");
//  cascade.Add("margin-right", "5px");
//
//  // We expect the pending substitution value for all the shorthands,
//  // except margin-right.
//  EXPECT_EQ("1px var(--x) 3px 4px", cascade.GetValue("margin-top"));
//  EXPECT_EQ("5px", cascade.GetValue("margin-right"));
//  EXPECT_EQ("1px var(--x) 3px 4px", cascade.GetValue("margin-bottom"));
//  EXPECT_EQ("1px var(--x) 3px 4px", cascade.GetValue("margin-left"));
//
//  // Apply a pending substitution value should modify the cascade for other
//  // longhands with the same pending substitution value.
//  cascade.Apply("margin-left");
//
//  EXPECT_EQ("1px", cascade.GetValue("margin-top"));
//  EXPECT_EQ("5px", cascade.GetValue("margin-right"));
//  EXPECT_EQ("3px", cascade.GetValue("margin-bottom"));
//  EXPECT_FALSE(cascade.GetValue("margin-left"));
//}

TEST_F(StyleCascadeTest, ResolverDetectCycle) {
  TestCascade cascade(GetDocument());
  TestCascadeResolver resolver(GetDocument());

  {
    TestCascadeAutoLock lock(CSSPropertyName("--a"), resolver);
    EXPECT_FALSE(resolver.InCycle());
    {
      TestCascadeAutoLock lock(CSSPropertyName("--b"), resolver);
      EXPECT_FALSE(resolver.InCycle());
      {
        TestCascadeAutoLock lock(CSSPropertyName("--c"), resolver);
        EXPECT_FALSE(resolver.InCycle());

        EXPECT_TRUE(resolver.DetectCycle("--a"));
        EXPECT_TRUE(resolver.InCycle());
      }
      EXPECT_TRUE(resolver.InCycle());
    }
    EXPECT_TRUE(resolver.InCycle());
  }
  EXPECT_FALSE(resolver.InCycle());
}

TEST_F(StyleCascadeTest, ResolverDetectNoCycle) {
  TestCascade cascade(GetDocument());
  TestCascadeResolver resolver(GetDocument());

  {
    TestCascadeAutoLock lock(CSSPropertyName("--a"), resolver);
    EXPECT_FALSE(resolver.InCycle());
    {
      TestCascadeAutoLock lock(CSSPropertyName("--b"), resolver);
      EXPECT_FALSE(resolver.InCycle());
      {
        TestCascadeAutoLock lock(CSSPropertyName("--c"), resolver);
        EXPECT_FALSE(resolver.InCycle());

        EXPECT_FALSE(resolver.DetectCycle("--x"));
        EXPECT_FALSE(resolver.InCycle());
      }
      EXPECT_FALSE(resolver.InCycle());
    }
    EXPECT_FALSE(resolver.InCycle());
  }
  EXPECT_FALSE(resolver.InCycle());
}

TEST_F(StyleCascadeTest, ResolverDetectCycleSelf) {
  TestCascade cascade(GetDocument());
  TestCascadeResolver resolver(GetDocument());

  {
    TestCascadeAutoLock lock(CSSPropertyName("--a"), resolver);
    EXPECT_FALSE(resolver.InCycle());

    EXPECT_TRUE(resolver.DetectCycle("--a"));
    EXPECT_TRUE(resolver.InCycle());
  }
  EXPECT_FALSE(resolver.InCycle());
}

TEST_F(StyleCascadeTest, ResolverDetectMultiCycle) {
  using AutoLock = TestCascadeAutoLock;

  TestCascade cascade(GetDocument());
  TestCascadeResolver resolver(GetDocument());

  {
    AutoLock lock(CSSPropertyName("--a"), resolver);
    EXPECT_FALSE(resolver.InCycle());
    {
      AutoLock lock(CSSPropertyName("--b"), resolver);
      EXPECT_FALSE(resolver.InCycle());
      {
        AutoLock lock(CSSPropertyName("--c"), resolver);
        EXPECT_FALSE(resolver.InCycle());
        {
          AutoLock lock(CSSPropertyName("--d"), resolver);
          EXPECT_FALSE(resolver.InCycle());

          // Cycle 1 (big cycle):
          EXPECT_TRUE(resolver.DetectCycle("--b"));
          EXPECT_TRUE(resolver.InCycle());
          EXPECT_EQ(1u, resolver.CycleDepth());

          // Cycle 2 (small cycle):
          EXPECT_TRUE(resolver.DetectCycle("--c"));
          EXPECT_TRUE(resolver.InCycle());
          EXPECT_EQ(1u, resolver.CycleDepth());
        }
      }
      EXPECT_TRUE(resolver.InCycle());
    }
    EXPECT_FALSE(resolver.InCycle());
  }
  EXPECT_FALSE(resolver.InCycle());
}

TEST_F(StyleCascadeTest, ResolverDetectMultiCycleReverse) {
  using AutoLock = TestCascadeAutoLock;

  TestCascade cascade(GetDocument());
  TestCascadeResolver resolver(GetDocument());

  {
    AutoLock lock(CSSPropertyName("--a"), resolver);
    EXPECT_FALSE(resolver.InCycle());
    {
      AutoLock lock(CSSPropertyName("--b"), resolver);
      EXPECT_FALSE(resolver.InCycle());
      {
        AutoLock lock(CSSPropertyName("--c"), resolver);
        EXPECT_FALSE(resolver.InCycle());
        {
          AutoLock lock(CSSPropertyName("--d"), resolver);
          EXPECT_FALSE(resolver.InCycle());

          // Cycle 1 (small cycle):
          EXPECT_TRUE(resolver.DetectCycle("--c"));
          EXPECT_TRUE(resolver.InCycle());
          EXPECT_EQ(2u, resolver.CycleDepth());

          // Cycle 2 (big cycle):
          EXPECT_TRUE(resolver.DetectCycle("--b"));
          EXPECT_TRUE(resolver.InCycle());
          EXPECT_EQ(1u, resolver.CycleDepth());
        }
      }
      EXPECT_TRUE(resolver.InCycle());
    }
    EXPECT_FALSE(resolver.InCycle());
  }
  EXPECT_FALSE(resolver.InCycle());
}

TEST_F(StyleCascadeTest, BasicCycle) {
  TestCascade cascade(GetDocument());
  cascade.Add("--a", "foo");
  cascade.Add("--b", "bar");
  cascade.Apply();

  EXPECT_EQ("foo", cascade.ComputedValue("--a"));
  EXPECT_EQ("bar", cascade.ComputedValue("--b"));

  cascade.Add("--a", "var(--b)");
  cascade.Add("--b", "var(--a)");
  cascade.Apply();

  EXPECT_FALSE(cascade.ComputedValue("--a"));
  EXPECT_FALSE(cascade.ComputedValue("--b"));
}

TEST_F(StyleCascadeTest, SelfCycle) {
  TestCascade cascade(GetDocument());
  cascade.Add("--a", "foo");
  cascade.Apply();

  EXPECT_EQ("foo", cascade.ComputedValue("--a"));

  cascade.Add("--a", "var(--a)");
  cascade.Apply();

  EXPECT_FALSE(cascade.ComputedValue("--a"));
}

TEST_F(StyleCascadeTest, SelfCycleInFallback) {
  TestCascade cascade(GetDocument());
  cascade.Add("--a", "var(--x, var(--a))");
  cascade.Apply();

  EXPECT_FALSE(cascade.ComputedValue("--a"));
}

TEST_F(StyleCascadeTest, SelfCycleInUnusedFallback) {
  TestCascade cascade(GetDocument());
  cascade.Add("--a", "var(--b, var(--a))");
  cascade.Add("--b", "10px");
  cascade.Apply();

  EXPECT_FALSE(cascade.ComputedValue("--a"));
  EXPECT_EQ("10px", cascade.ComputedValue("--b"));
}

TEST_F(StyleCascadeTest, LongCycle) {
  TestCascade cascade(GetDocument());
  cascade.Add("--a", "var(--b)");
  cascade.Add("--b", "var(--c)");
  cascade.Add("--c", "var(--d)");
  cascade.Add("--d", "var(--e)");
  cascade.Add("--e", "var(--a)");
  cascade.Apply();

  EXPECT_FALSE(cascade.ComputedValue("--a"));
  EXPECT_FALSE(cascade.ComputedValue("--b"));
  EXPECT_FALSE(cascade.ComputedValue("--c"));
  EXPECT_FALSE(cascade.ComputedValue("--d"));
  EXPECT_FALSE(cascade.ComputedValue("--e"));
}

TEST_F(StyleCascadeTest, PartialCycle) {
  TestCascade cascade(GetDocument());
  cascade.Add("--a", "var(--b)");
  cascade.Add("--b", "var(--a)");
  cascade.Add("--c", "bar var(--d) var(--a)");
  cascade.Add("--d", "foo");
  cascade.Apply();

  EXPECT_FALSE(cascade.ComputedValue("--a"));
  EXPECT_FALSE(cascade.ComputedValue("--b"));
  EXPECT_FALSE(cascade.ComputedValue("--c"));
  EXPECT_EQ("foo", cascade.ComputedValue("--d"));
}

TEST_F(StyleCascadeTest, VarCycleViaFallback) {
  TestCascade cascade(GetDocument());
  cascade.Add("--a", "var(--b)");
  cascade.Add("--b", "var(--x, var(--a))");
  cascade.Add("--c", "var(--a)");
  cascade.Apply();

  EXPECT_FALSE(cascade.ComputedValue("--a"));
  EXPECT_FALSE(cascade.ComputedValue("--b"));
  EXPECT_FALSE(cascade.ComputedValue("--c"));
}

TEST_F(StyleCascadeTest, FallbackTriggeredByCycle) {
  TestCascade cascade(GetDocument());
  cascade.Add("--a", "var(--b)");
  cascade.Add("--b", "var(--a)");
  cascade.Add("--c", "var(--a,foo)");
  cascade.Apply();

  EXPECT_FALSE(cascade.ComputedValue("--a"));
  EXPECT_FALSE(cascade.ComputedValue("--b"));
  EXPECT_EQ("foo", cascade.ComputedValue("--c"));
}

TEST_F(StyleCascadeTest, RegisteredCycle) {
  RegisterProperty(GetDocument(), "--a", "<length>", "0px", false);
  RegisterProperty(GetDocument(), "--b", "<length>", "0px", false);

  TestCascade cascade(GetDocument());
  cascade.Add("--a", "var(--b)");
  cascade.Add("--b", "var(--a)");
  cascade.Apply();

  EXPECT_FALSE(cascade.ComputedValue("--a"));
  EXPECT_FALSE(cascade.ComputedValue("--b"));
}

TEST_F(StyleCascadeTest, PartiallyRegisteredCycle) {
  RegisterProperty(GetDocument(), "--a", "<length>", "0px", false);

  TestCascade cascade(GetDocument());
  cascade.Add("--a", "var(--b)");
  cascade.Add("--b", "var(--a)");
  cascade.Apply();

  EXPECT_FALSE(cascade.ComputedValue("--a"));
  EXPECT_FALSE(cascade.ComputedValue("--b"));
}

TEST_F(StyleCascadeTest, FallbackTriggeredByRegisteredCycle) {
  RegisterProperty(GetDocument(), "--a", "<length>", "0px", false);
  RegisterProperty(GetDocument(), "--b", "<length>", "0px", false);

  TestCascade cascade(GetDocument());
  // Cycle:
  cascade.Add("--a", "var(--b)");
  cascade.Add("--b", "var(--a)");
  // References to cycle:
  cascade.Add("--c", "var(--a,1px)");
  cascade.Add("--d", "var(--b,2px)");
  cascade.Apply();

  EXPECT_FALSE(cascade.ComputedValue("--a"));
  EXPECT_FALSE(cascade.ComputedValue("--b"));
  EXPECT_EQ("1px", cascade.ComputedValue("--c"));
  EXPECT_EQ("2px", cascade.ComputedValue("--d"));
}

TEST_F(StyleCascadeTest, CycleStillInvalidWithFallback) {
  TestCascade cascade(GetDocument());
  // Cycle:
  cascade.Add("--a", "var(--b,red)");
  cascade.Add("--b", "var(--a,red)");
  // References to cycle:
  cascade.Add("--c", "var(--a,green)");
  cascade.Add("--d", "var(--b,green)");
  cascade.Apply();

  EXPECT_FALSE(cascade.ComputedValue("--a"));
  EXPECT_FALSE(cascade.ComputedValue("--b"));
  EXPECT_EQ("green", cascade.ComputedValue("--c"));
  EXPECT_EQ("green", cascade.ComputedValue("--d"));
}

TEST_F(StyleCascadeTest, CycleInFallbackStillInvalid) {
  TestCascade cascade(GetDocument());
  // Cycle:
  cascade.Add("--a", "var(--b,red)");
  cascade.Add("--b", "var(--x,var(--a))");
  // References to cycle:
  cascade.Add("--c", "var(--a,green)");
  cascade.Add("--d", "var(--b,green)");
  cascade.Apply();

  EXPECT_FALSE(cascade.ComputedValue("--a"));
  EXPECT_FALSE(cascade.ComputedValue("--b"));
  EXPECT_EQ("green", cascade.ComputedValue("--c"));
  EXPECT_EQ("green", cascade.ComputedValue("--d"));
}

TEST_F(StyleCascadeTest, CycleMultiple) {
  TestCascade cascade(GetDocument());
  // Cycle:
  cascade.Add("--a", "var(--c, red)");
  cascade.Add("--b", "var(--c, red)");
  cascade.Add("--c", "var(--a, blue) var(--b, blue)");
  // References to cycle:
  cascade.Add("--d", "var(--a,green)");
  cascade.Add("--e", "var(--b,green)");
  cascade.Add("--f", "var(--c,green)");
  cascade.Apply();

  EXPECT_FALSE(cascade.ComputedValue("--a"));
  EXPECT_FALSE(cascade.ComputedValue("--b"));
  EXPECT_FALSE(cascade.ComputedValue("--c"));
  EXPECT_EQ("green", cascade.ComputedValue("--d"));
  EXPECT_EQ("green", cascade.ComputedValue("--e"));
  EXPECT_EQ("green", cascade.ComputedValue("--f"));
}

TEST_F(StyleCascadeTest, CycleMultipleFallback) {
  TestCascade cascade(GetDocument());
  // Cycle:
  cascade.Add("--a", "var(--b, red)");
  cascade.Add("--b", "var(--a, var(--c, red))");
  cascade.Add("--c", "var(--b, red)");
  // References to cycle:
  cascade.Add("--d", "var(--a,green)");
  cascade.Add("--e", "var(--b,green)");
  cascade.Add("--f", "var(--c,green)");
  cascade.Apply();

  EXPECT_FALSE(cascade.ComputedValue("--a"));
  EXPECT_FALSE(cascade.ComputedValue("--b"));
  EXPECT_FALSE(cascade.ComputedValue("--c"));
  EXPECT_EQ("green", cascade.ComputedValue("--d"));
  EXPECT_EQ("green", cascade.ComputedValue("--e"));
  EXPECT_EQ("green", cascade.ComputedValue("--f"));
}

TEST_F(StyleCascadeTest, CycleMultipleUnusedFallback) {
  TestCascade cascade(GetDocument());
  cascade.Add("--a", "red");
  // Cycle:
  cascade.Add("--b", "var(--c, red)");
  cascade.Add("--c", "var(--a, var(--b, red) var(--d, red))");
  cascade.Add("--d", "var(--c, red)");
  // References to cycle:
  cascade.Add("--e", "var(--b,green)");
  cascade.Add("--f", "var(--c,green)");
  cascade.Add("--g", "var(--d,green)");
  cascade.Apply();

  EXPECT_FALSE(cascade.ComputedValue("--b"));
  EXPECT_FALSE(cascade.ComputedValue("--c"));
  EXPECT_FALSE(cascade.ComputedValue("--d"));
  EXPECT_EQ("green", cascade.ComputedValue("--e"));
  EXPECT_EQ("green", cascade.ComputedValue("--f"));
  EXPECT_EQ("green", cascade.ComputedValue("--g"));
}

TEST_F(StyleCascadeTest, CycleReferencedFromStandardProperty) {
  TestCascade cascade(GetDocument());
  cascade.Add("--a", "var(--b)");
  cascade.Add("--b", "var(--a)");
  cascade.Add("color:var(--a,green)");
  cascade.Apply();

  EXPECT_FALSE(cascade.ComputedValue("--a"));
  EXPECT_FALSE(cascade.ComputedValue("--b"));
  EXPECT_EQ("rgb(0, 128, 0)", cascade.ComputedValue("color"));
}

TEST_F(StyleCascadeTest, CycleReferencedFromShorthand) {
  TestCascade cascade(GetDocument());
  cascade.Add("--a", "var(--b)");
  cascade.Add("--b", "var(--a)");
  cascade.Add("background", "var(--a,green)");
  cascade.Apply();

  EXPECT_FALSE(cascade.ComputedValue("--a"));
  EXPECT_FALSE(cascade.ComputedValue("--b"));
  EXPECT_EQ("rgb(0, 128, 0)", cascade.ComputedValue("background-color"));
}

TEST_F(StyleCascadeTest, EmUnit) {
  TestCascade cascade(GetDocument());
  cascade.Add("font-size", "10px");
  cascade.Add("width", "10em");
  cascade.Apply();

  EXPECT_EQ("100px", cascade.ComputedValue("width"));
}

TEST_F(StyleCascadeTest, EmUnitCustomProperty) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);

  TestCascade cascade(GetDocument());
  cascade.Add("font-size", "10px");
  cascade.Add("--x", "10em");
  cascade.Apply();

  EXPECT_EQ("100px", cascade.ComputedValue("--x"));
}

TEST_F(StyleCascadeTest, EmUnitNonCycle) {
  TestCascade parent(GetDocument());
  parent.Add("font-size", "10px");
  parent.Apply();

  TestCascade cascade(GetDocument());
  cascade.InheritFrom(parent.TakeStyle());
  cascade.Add("font-size", "var(--x)");
  cascade.Add("--x", "10em");
  cascade.Apply();

  // Note: Only registered properties can have cycles with font-size.
  EXPECT_EQ("100px", cascade.ComputedValue("font-size"));
}

TEST_F(StyleCascadeTest, EmUnitCycle) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);

  TestCascade cascade(GetDocument());
  cascade.Add("font-size", "var(--x)");
  cascade.Add("--x", "10em");
  cascade.Apply();

  EXPECT_FALSE(cascade.ComputedValue("--x"));
}

TEST_F(StyleCascadeTest, SubstitutingEmCycles) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);

  TestCascade cascade(GetDocument());
  cascade.Add("font-size", "var(--x)");
  cascade.Add("--x", "10em");
  cascade.Add("--y", "var(--x)");
  cascade.Add("--z", "var(--x,1px)");
  cascade.Apply();

  EXPECT_FALSE(cascade.ComputedValue("--y"));
  EXPECT_EQ("1px", cascade.ComputedValue("--z"));
}

TEST_F(StyleCascadeTest, RemUnit) {
  SetRootFont("10px");
  UpdateAllLifecyclePhasesForTest();

  TestCascade cascade(GetDocument());
  cascade.Add("width", "10rem");
  cascade.Apply();

  EXPECT_EQ("100px", cascade.ComputedValue("width"));
}

TEST_F(StyleCascadeTest, RemUnitCustomProperty) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);

  SetRootFont("10px");
  UpdateAllLifecyclePhasesForTest();

  TestCascade cascade(GetDocument());
  cascade.Add("--x", "10rem");
  cascade.Apply();

  EXPECT_EQ("100px", cascade.ComputedValue("--x"));
}

TEST_F(StyleCascadeTest, RemUnitInFontSize) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);

  SetRootFont("10px");
  UpdateAllLifecyclePhasesForTest();

  TestCascade cascade(GetDocument());
  cascade.Add("font-size", "1rem");
  cascade.Add("--x", "10rem");
  cascade.Apply();

  EXPECT_EQ("100px", cascade.ComputedValue("--x"));
}

TEST_F(StyleCascadeTest, RemUnitInRootFontSizeCycle) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);

  TestCascade cascade(GetDocument(), DocumentElement());
  cascade.Add("font-size", "var(--x)");
  cascade.Add("--x", "1rem");
  cascade.Apply();

  EXPECT_FALSE(cascade.ComputedValue("--x"));
}

TEST_F(StyleCascadeTest, RemUnitInRootFontSizeNonCycle) {
  TestCascade cascade(GetDocument(), DocumentElement());
  cascade.Add("font-size", "initial");
  cascade.Apply();

  String expected = cascade.ComputedValue("font-size");

  cascade.Add("font-size", "var(--x)");
  cascade.Add("--x", "1rem");
  cascade.Apply();

  // Note: Only registered properties can have cycles with font-size.
  EXPECT_EQ("1rem", cascade.ComputedValue("--x"));
  EXPECT_EQ(expected, cascade.ComputedValue("font-size"));
}

TEST_F(StyleCascadeTest, Initial) {
  TestCascade parent(GetDocument());
  parent.Add("--x", "foo");
  parent.Apply();

  TestCascade cascade(GetDocument());
  cascade.InheritFrom(parent.TakeStyle());
  cascade.Add("--y", "foo");
  cascade.Apply();

  EXPECT_EQ("foo", cascade.ComputedValue("--x"));
  EXPECT_EQ("foo", cascade.ComputedValue("--y"));

  cascade.Add("--x", "initial");
  cascade.Add("--y", "initial");
  cascade.Apply();

  EXPECT_FALSE(cascade.ComputedValue("--x"));
  EXPECT_FALSE(cascade.ComputedValue("--y"));
}

TEST_F(StyleCascadeTest, Inherit) {
  TestCascade parent(GetDocument());
  parent.Add("--x", "foo");
  parent.Apply();

  TestCascade cascade(GetDocument());
  cascade.InheritFrom(parent.TakeStyle());

  EXPECT_EQ("foo", cascade.ComputedValue("--x"));

  cascade.Add("--x", "bar");
  cascade.Apply();
  EXPECT_EQ("bar", cascade.ComputedValue("--x"));

  cascade.Add("--x", "inherit");
  cascade.Apply();
  EXPECT_EQ("foo", cascade.ComputedValue("--x"));
}

TEST_F(StyleCascadeTest, Unset) {
  TestCascade parent(GetDocument());
  parent.Add("--x", "foo");
  parent.Apply();

  TestCascade cascade(GetDocument());
  cascade.InheritFrom(parent.TakeStyle());
  EXPECT_EQ("foo", cascade.ComputedValue("--x"));

  cascade.Add("--x", "bar");
  cascade.Apply();
  EXPECT_EQ("bar", cascade.ComputedValue("--x"));

  cascade.Add("--x", "unset");
  cascade.Apply();
  EXPECT_EQ("foo", cascade.ComputedValue("--x"));
}

TEST_F(StyleCascadeTest, RegisteredInitial) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);

  TestCascade cascade(GetDocument());
  cascade.Apply();
  EXPECT_EQ("0px", cascade.ComputedValue("--x"));
}

TEST_F(StyleCascadeTest, SubstituteRegisteredImplicitInitialValue) {
  RegisterProperty(GetDocument(), "--x", "<length>", "13px", false);

  TestCascade cascade(GetDocument());
  cascade.Add("--y", " var(--x) ");
  cascade.Apply();
  EXPECT_EQ("13px", cascade.ComputedValue("--x"));
  EXPECT_EQ(" 13px ", cascade.ComputedValue("--y"));
}

TEST_F(StyleCascadeTest, SubstituteRegisteredUniversal) {
  RegisterProperty(GetDocument(), "--x", "*", "foo", false);

  TestCascade cascade(GetDocument());
  cascade.Add("--x", "bar");
  cascade.Add("--y", "var(--x)");
  cascade.Apply();
  EXPECT_EQ("bar", cascade.ComputedValue("--x"));
  EXPECT_EQ("bar", cascade.ComputedValue("--y"));
}

TEST_F(StyleCascadeTest, SubstituteRegisteredUniversalInvalid) {
  RegisterProperty(GetDocument(), "--x", "*", g_null_atom, false);

  TestCascade cascade(GetDocument());
  cascade.Add("--y", " var(--x) ");
  cascade.Apply();
  EXPECT_FALSE(cascade.ComputedValue("--x"));
  EXPECT_FALSE(cascade.ComputedValue("--y"));
}

TEST_F(StyleCascadeTest, SubstituteRegisteredUniversalInitial) {
  RegisterProperty(GetDocument(), "--x", "*", "foo", false);

  TestCascade cascade(GetDocument());
  cascade.Add("--y", " var(--x) ");
  cascade.Apply();
  EXPECT_EQ("foo", cascade.ComputedValue("--x"));
  EXPECT_EQ(" foo ", cascade.ComputedValue("--y"));
}

TEST_F(StyleCascadeTest, RegisteredExplicitInitial) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);

  TestCascade cascade(GetDocument());
  cascade.Add("--x", "10px");
  cascade.Apply();
  EXPECT_EQ("10px", cascade.ComputedValue("--x"));

  cascade.Add("--x", "initial");
  cascade.Add("--y", "var(--x)");
  cascade.Apply();
  EXPECT_EQ("0px", cascade.ComputedValue("--x"));
  EXPECT_EQ("0px", cascade.ComputedValue("--y"));
}

TEST_F(StyleCascadeTest, RegisteredExplicitInherit) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);

  TestCascade parent(GetDocument());
  parent.Add("--x", "15px");
  parent.Apply();
  EXPECT_EQ("15px", parent.ComputedValue("--x"));

  TestCascade cascade(GetDocument());
  cascade.InheritFrom(parent.TakeStyle());
  cascade.Apply();
  EXPECT_EQ("0px", cascade.ComputedValue("--x"));  // Note: inherit==false

  cascade.Add("--x", "inherit");
  cascade.Add("--y", "var(--x)");
  cascade.Apply();
  EXPECT_EQ("15px", cascade.ComputedValue("--x"));
  EXPECT_EQ("15px", cascade.ComputedValue("--y"));
}

TEST_F(StyleCascadeTest, RegisteredExplicitUnset) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);
  RegisterProperty(GetDocument(), "--y", "<length>", "0px", true);

  TestCascade parent(GetDocument());
  parent.Add("--x", "15px");
  parent.Add("--y", "15px");
  parent.Apply();
  EXPECT_EQ("15px", parent.ComputedValue("--x"));
  EXPECT_EQ("15px", parent.ComputedValue("--y"));

  TestCascade cascade(GetDocument());
  cascade.InheritFrom(parent.TakeStyle());
  cascade.Add("--x", "2px");
  cascade.Add("--y", "2px");
  cascade.Apply();
  EXPECT_EQ("2px", cascade.ComputedValue("--x"));
  EXPECT_EQ("2px", cascade.ComputedValue("--y"));

  cascade.Add("--x", "unset");
  cascade.Add("--y", "unset");
  cascade.Add("--z", "var(--x) var(--y)");
  cascade.Apply();
  EXPECT_EQ("0px", cascade.ComputedValue("--x"));
  EXPECT_EQ("15px", cascade.ComputedValue("--y"));
  EXPECT_EQ("0px 15px", cascade.ComputedValue("--z"));
}

TEST_F(StyleCascadeTest, SubstituteAnimationTaintedInCustomProperty) {
  TestCascade cascade(GetDocument());
  cascade.Add(AnimationTaintedSet("--x", "15px"));
  cascade.Add("--y", "var(--x)");
  cascade.Apply();
  EXPECT_EQ("15px", cascade.ComputedValue("--x"));
  EXPECT_EQ("15px", cascade.ComputedValue("--y"));
}

TEST_F(StyleCascadeTest, SubstituteAnimationTaintedInStandardProperty) {
  TestCascade cascade(GetDocument());
  cascade.Add(AnimationTaintedSet("--x", "15px"));
  cascade.Add("width", "var(--x)");
  cascade.Apply();
  EXPECT_EQ("15px", cascade.ComputedValue("--x"));
  EXPECT_EQ("15px", cascade.ComputedValue("width"));
}

TEST_F(StyleCascadeTest, SubstituteAnimationTaintedInAnimationProperty) {
  TestCascade cascade(GetDocument());
  cascade.Add("--x", "20s");
  cascade.Add("animation-duration", "var(--x)");
  cascade.Apply();

  EXPECT_EQ("20s", cascade.ComputedValue("--x"));
  EXPECT_EQ("20s", cascade.ComputedValue("animation-duration"));

  cascade.Add(AnimationTaintedSet("--y", "20s"));
  cascade.Add("animation-duration", "var(--y)");
  cascade.Apply();

  EXPECT_EQ("20s", cascade.ComputedValue("--y"));
  EXPECT_EQ("0s", cascade.ComputedValue("animation-duration"));
}

TEST_F(StyleCascadeTest, IndirectlyAnimationTainted) {
  TestCascade cascade(GetDocument());
  cascade.Add(AnimationTaintedSet("--x", "20s"));
  cascade.Add("--y", "var(--x)");
  cascade.Add("animation-duration", "var(--y)");
  cascade.Apply();

  EXPECT_EQ("20s", cascade.ComputedValue("--x"));
  EXPECT_EQ("20s", cascade.ComputedValue("--y"));
  EXPECT_EQ("0s", cascade.ComputedValue("animation-duration"));
}

TEST_F(StyleCascadeTest, AnimationTaintedFallback) {
  TestCascade cascade(GetDocument());
  cascade.Add(AnimationTaintedSet("--x", "20s"));
  cascade.Add("animation-duration", "var(--x,1s)");
  cascade.Apply();

  EXPECT_EQ("20s", cascade.ComputedValue("--x"));
  EXPECT_EQ("1s", cascade.ComputedValue("animation-duration"));
}

TEST_F(StyleCascadeTest, EnvMissingNestedVar) {
  TestCascade cascade(GetDocument());
  cascade.Add("--x", "rgb(0, 0, 0)");
  cascade.Add("background-color", "env(missing, var(--x))");
  cascade.Apply();

  EXPECT_EQ("rgb(0, 0, 0)", cascade.ComputedValue("--x"));
  EXPECT_EQ("rgb(0, 0, 0)", cascade.ComputedValue("background-color"));
}

TEST_F(StyleCascadeTest, EnvMissingNestedVarFallback) {
  TestCascade cascade(GetDocument());
  cascade.Add("background-color", "env(missing, var(--missing, blue))");
  cascade.Apply();

  EXPECT_EQ("rgb(0, 0, 255)", cascade.ComputedValue("background-color"));
}

TEST_F(StyleCascadeTest, EnvMissingFallback) {
  TestCascade cascade(GetDocument());
  cascade.Add("background-color", "env(missing, blue)");
  cascade.Apply();

  EXPECT_EQ("rgb(0, 0, 255)", cascade.ComputedValue("background-color"));
}

TEST_F(StyleCascadeTest, ValidEnv) {
  AutoEnv env(*this, "test", "red");

  TestCascade cascade(GetDocument());
  cascade.Add("background-color", "env(test, blue)");
  cascade.Apply();

  EXPECT_EQ("rgb(255, 0, 0)", cascade.ComputedValue("background-color"));
}

TEST_F(StyleCascadeTest, ValidEnvFallback) {
  AutoEnv env(*this, "test", "red");

  TestCascade cascade(GetDocument());
  cascade.Add("background-color", "env(test, blue)");
  cascade.Apply();

  EXPECT_EQ("rgb(255, 0, 0)", cascade.ComputedValue("background-color"));
}

TEST_F(StyleCascadeTest, ValidEnvInUnusedFallback) {
  AutoEnv env(*this, "test", "red");

  TestCascade cascade(GetDocument());
  cascade.Add("--x", "rgb(0, 0, 0)");
  cascade.Add("background-color", "var(--x, env(test))");
  cascade.Apply();

  EXPECT_EQ("rgb(0, 0, 0)", cascade.ComputedValue("--x"));
  EXPECT_EQ("rgb(0, 0, 0)", cascade.ComputedValue("background-color"));
}

TEST_F(StyleCascadeTest, ValidEnvInUsedFallback) {
  AutoEnv env(*this, "test", "red");

  TestCascade cascade(GetDocument());
  cascade.Add("background-color", "var(--missing, env(test))");
  cascade.Apply();

  EXPECT_EQ("rgb(255, 0, 0)", cascade.ComputedValue("background-color"));
}

TEST_F(StyleCascadeTest, PendingKeyframeAnimation) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);

  AppendSheet(R"HTML(
     @keyframes test {
        from { --x: 10px; }
        to { --x: 20px; }
     }
    )HTML");

  TestCascade cascade(GetDocument());

  cascade.Add("animation-name", "test");
  cascade.Add("animation-duration", "1s");
  cascade.Apply();

  cascade.AnalyzeAnimations();

  EXPECT_EQ(CascadeOrigin::kAnimation, cascade.GetPriority("--x").GetOrigin());
}

TEST_F(StyleCascadeTest, PendingKeyframeAnimationApply) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);

  AppendSheet(R"HTML(
     @keyframes test {
        from { --x: 10px; }
        to { --x: 20px; }
     }
    )HTML");

  TestCascade cascade(GetDocument());

  cascade.Add("animation-name", "test");
  cascade.Add("animation-duration", "10s");
  cascade.Add("animation-timing-function", "linear");
  cascade.Add("animation-delay", "-5s");
  cascade.Apply();

  cascade.AnalyzeAnimations();

  EXPECT_EQ(CascadeOrigin::kAnimation, cascade.GetPriority("--x").GetOrigin());
  cascade.Apply();
  EXPECT_EQ("15px", cascade.ComputedValue("--x"));
}

TEST_F(StyleCascadeTest, TransitionCausesInterpolationValue) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);

  // First, simulate an "old style".
  TestCascade cascade1(GetDocument());
  cascade1.Add("--x", "10px");
  cascade1.Add("transition", "--x 1s");
  cascade1.Apply();

  // Set the old style on the element, so that the animation
  // update detects it.
  GetDocument().body()->SetComputedStyle(cascade1.TakeStyle());

  // Now simulate a new style, with a new value for --x.
  TestCascade cascade2(GetDocument());
  cascade2.Add("--x", "20px");
  cascade2.Add("transition", "--x 1s");
  cascade2.Apply();

  cascade2.AnalyzeTransitions();

  EXPECT_EQ(CascadeOrigin::kTransition,
            cascade2.GetPriority("--x").GetOrigin());
}

TEST_F(StyleCascadeTest, TransitionDetectedForChangedFontSize) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);

  TestCascade cascade1(GetDocument());
  cascade1.Add("font-size", "10px");
  cascade1.Add("--x", "10em");
  cascade1.Add("width", "10em");
  cascade1.Add("height", "10px");
  cascade1.Add("transition", "--x 1s, width 1s");
  cascade1.Apply();

  GetDocument().body()->SetComputedStyle(cascade1.TakeStyle());

  TestCascade cascade2(GetDocument());
  cascade2.Add("font-size", "20px");
  cascade2.Add("--x", "10em");
  cascade2.Add("width", "10em");
  cascade2.Add("height", "10px");
  cascade2.Add("transition", "--x 1s, width 1s");
  cascade2.Apply();

  cascade2.AnalyzeTransitions();

  EXPECT_EQ(CascadeOrigin::kTransition, cascade2.GetOrigin("--x"));
  EXPECT_EQ(CascadeOrigin::kTransition, cascade2.GetOrigin("width"));
  EXPECT_EQ("10px", cascade2.ComputedValue("height"));
}

TEST_F(StyleCascadeTest, AnimatingVarReferences) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);

  AppendSheet(R"HTML(
     @keyframes test {
        from { --x: var(--from); }
        to { --x: var(--to); }
     }
    )HTML");

  TestCascade cascade(GetDocument());

  cascade.Add("animation-name", "test");
  cascade.Add("animation-duration", "10s");
  cascade.Add("animation-timing-function", "linear");
  cascade.Add("animation-delay", "-5s");
  cascade.Apply();

  cascade.AnalyzeAnimations();
  cascade.Add("--from", "10px");
  cascade.Add("--to", "20px");
  cascade.Add("--y", "var(--x)");
  cascade.Apply();

  EXPECT_EQ("15px", cascade.ComputedValue("--x"));
  EXPECT_EQ("15px", cascade.ComputedValue("--y"));
}

TEST_F(StyleCascadeTest, AnimateStandardProperty) {
  AppendSheet(R"HTML(
     @keyframes test {
        from { width: 10px; }
        to { width: 20px; }
     }
    )HTML");

  TestCascade cascade(GetDocument());

  cascade.Add("animation-name", "test");
  cascade.Add("animation-duration", "10s");
  cascade.Add("animation-timing-function", "linear");
  cascade.Add("animation-delay", "-5s");
  cascade.Apply();

  cascade.AnalyzeAnimations();
  EXPECT_EQ(CascadeOrigin::kAnimation, cascade.GetOrigin("width"));

  cascade.Apply();
  EXPECT_EQ("15px", cascade.ComputedValue("width"));
}

TEST_F(StyleCascadeTest, AuthorImportantWinOverAnimations) {
  AppendSheet(R"HTML(
     @keyframes test {
        from { width: 10px; height: 10px; }
        to { width: 20px; height: 20px; }
     }
    )HTML");

  TestCascade cascade(GetDocument());

  cascade.Add("animation-name", "test");
  cascade.Add("animation-duration", "10s");
  cascade.Add("animation-timing-function", "linear");
  cascade.Add("animation-delay", "-5s");
  cascade.Add("width:40px");
  cascade.Add("height:40px !important");
  cascade.Apply();

  cascade.AnalyzeAnimations();
  EXPECT_EQ(CascadeOrigin::kAnimation, cascade.GetOrigin("width"));
  EXPECT_EQ(CascadeOrigin::kAuthor, cascade.GetOrigin("height"));

  cascade.Apply();
  EXPECT_EQ("15px", cascade.ComputedValue("width"));
  EXPECT_EQ("40px", cascade.ComputedValue("height"));
}

TEST_F(StyleCascadeTest, TransitionsWinOverAuthorImportant) {
  // First, simulate an "old style".
  TestCascade cascade1(GetDocument());
  cascade1.Add("width:10px !important");
  cascade1.Add("height:10px !important");
  cascade1.Add("transition:all 1s");
  cascade1.Apply();

  // Set the old style on the element, so that the animation
  // update detects it.
  GetDocument().body()->SetComputedStyle(cascade1.TakeStyle());

  // Now simulate a new style, with a new value for width/height.
  TestCascade cascade2(GetDocument());
  cascade2.Add("width:20px !important");
  cascade2.Add("height:20px !important");
  cascade2.Add("transition:all 1s");
  cascade2.Apply();

  cascade2.AnalyzeTransitions();
  cascade2.Apply();

  EXPECT_EQ(CascadeOrigin::kTransition,
            cascade2.GetPriority("width").GetOrigin());
  EXPECT_EQ(CascadeOrigin::kTransition,
            cascade2.GetPriority("height").GetOrigin());
}

TEST_F(StyleCascadeTest, EmRespondsToAnimatedFontSize) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);

  AppendSheet(R"HTML(
     @keyframes test {
        from { font-size: 10px; }
        to { font-size: 20px; }
     }
    )HTML");

  TestCascade cascade(GetDocument());

  cascade.Add("animation-name", "test");
  cascade.Add("animation-duration", "10s");
  cascade.Add("animation-timing-function", "linear");
  cascade.Add("animation-delay", "-5s");
  cascade.Apply();

  cascade.AnalyzeAnimations();
  cascade.Add("--x", "2em");
  cascade.Add("width", "10em");

  cascade.Apply();
  EXPECT_EQ("30px", cascade.ComputedValue("--x"));
  EXPECT_EQ("150px", cascade.ComputedValue("width"));
}

TEST_F(StyleCascadeTest, AnimateStandardPropertyWithVar) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);

  AppendSheet(R"HTML(
     @keyframes test {
        from { width: var(--from); }
        to { width: var(--to); }
     }
    )HTML");

  TestCascade cascade(GetDocument());

  cascade.Add("animation-name", "test");
  cascade.Add("animation-duration", "10s");
  cascade.Add("animation-timing-function", "linear");
  cascade.Add("animation-delay", "-5s");
  cascade.Apply();

  cascade.AnalyzeAnimations();
  cascade.Add("--from", "10px");
  cascade.Add("--to", "20px");

  cascade.Apply();
  EXPECT_EQ("15px", cascade.ComputedValue("width"));
}

TEST_F(StyleCascadeTest, AnimateStandardShorthand) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);

  AppendSheet(R"HTML(
     @keyframes test {
        from { margin: 10px; }
        to { margin: 20px; }
     }
    )HTML");

  TestCascade cascade(GetDocument());

  cascade.Add("animation-name", "test");
  cascade.Add("animation-duration", "10s");
  cascade.Add("animation-timing-function", "linear");
  cascade.Add("animation-delay", "-5s");
  cascade.Apply();

  cascade.AnalyzeAnimations();
  EXPECT_EQ(CascadeOrigin::kAnimation, cascade.GetOrigin("margin-top"));
  EXPECT_EQ(CascadeOrigin::kAnimation, cascade.GetOrigin("margin-right"));
  EXPECT_EQ(CascadeOrigin::kAnimation, cascade.GetOrigin("margin-bottom"));
  EXPECT_EQ(CascadeOrigin::kAnimation, cascade.GetOrigin("margin-left"));

  cascade.Apply();
  EXPECT_EQ("15px", cascade.ComputedValue("margin-top"));
  EXPECT_EQ("15px", cascade.ComputedValue("margin-right"));
  EXPECT_EQ("15px", cascade.ComputedValue("margin-bottom"));
  EXPECT_EQ("15px", cascade.ComputedValue("margin-left"));
}

TEST_F(StyleCascadeTest, AnimatePendingSubstitutionValue) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);

  AppendSheet(R"HTML(
     @keyframes test {
        from { margin: var(--from); }
        to { margin: var(--to); }
     }
    )HTML");

  TestCascade cascade(GetDocument());

  cascade.Add("animation-name", "test");
  cascade.Add("animation-duration", "10s");
  cascade.Add("animation-timing-function", "linear");
  cascade.Add("animation-delay", "-5s");
  cascade.Apply();

  cascade.AnalyzeAnimations();
  cascade.Add("--from", "10px");
  cascade.Add("--to", "20px");

  EXPECT_EQ(CascadeOrigin::kAnimation, cascade.GetOrigin("margin-top"));
  EXPECT_EQ(CascadeOrigin::kAnimation, cascade.GetOrigin("margin-right"));
  EXPECT_EQ(CascadeOrigin::kAnimation, cascade.GetOrigin("margin-bottom"));
  EXPECT_EQ(CascadeOrigin::kAnimation, cascade.GetOrigin("margin-left"));

  cascade.Apply();
  EXPECT_EQ("15px", cascade.ComputedValue("margin-top"));
  EXPECT_EQ("15px", cascade.ComputedValue("margin-right"));
  EXPECT_EQ("15px", cascade.ComputedValue("margin-bottom"));
  EXPECT_EQ("15px", cascade.ComputedValue("margin-left"));
}

TEST_F(StyleCascadeTest, ForeignObjectZoomVsEffectiveZoom) {
  GetDocument().body()->SetInnerHTMLFromString(R"HTML(
    <svg>
      <foreignObject id='foreign'></foreignObject>
    </svg>
  )HTML");
  UpdateAllLifecyclePhasesForTest();

  Element* foreign_object = GetDocument().getElementById("foreign");
  ASSERT_TRUE(foreign_object);

  TestCascade cascade(GetDocument(), foreign_object);
  cascade.Add("-internal-effective-zoom:initial !important",
              CascadeOrigin::kUserAgent);
  cascade.Add("zoom:200%");
  cascade.Apply();

  EXPECT_EQ(1.0f, cascade.TakeStyle()->EffectiveZoom());
}

TEST_F(StyleCascadeTest, ZoomCascadeOrder) {
  TestCascade cascade(GetDocument());
  cascade.Add("zoom:200%", CascadeOrigin::kUserAgent);
  cascade.Add("-internal-effective-zoom:initial", CascadeOrigin::kUserAgent);
  cascade.Apply();

  EXPECT_EQ(1.0f, cascade.TakeStyle()->EffectiveZoom());
}

TEST_F(StyleCascadeTest, ZoomVsAll) {
  TestCascade cascade(GetDocument());
  cascade.Add("zoom:200%", CascadeOrigin::kUserAgent);
  cascade.Add("all:initial");
  cascade.Apply();

  EXPECT_EQ(1.0f, cascade.TakeStyle()->EffectiveZoom());
}

TEST_F(StyleCascadeTest, InternalEffectiveZoomVsAll) {
  TestCascade cascade(GetDocument());
  cascade.Add("-internal-effective-zoom:200%", CascadeOrigin::kUserAgent);
  cascade.Add("all:initial");
  cascade.Apply();

  EXPECT_EQ(1.0f, cascade.TakeStyle()->EffectiveZoom());
}

TEST_F(StyleCascadeTest, ZoomReversedCascadeOrder) {
  TestCascade cascade(GetDocument());
  cascade.Add("-internal-effective-zoom:initial", CascadeOrigin::kUserAgent);
  cascade.Add("zoom:200%", CascadeOrigin::kUserAgent);
  cascade.Apply();

  EXPECT_EQ(2.0f, cascade.TakeStyle()->EffectiveZoom());
}

TEST_F(StyleCascadeTest, ZoomImportant) {
  TestCascade cascade(GetDocument());
  cascade.Add("zoom:200% !important", CascadeOrigin::kUserAgent);
  cascade.Add("-internal-effective-zoom:initial", CascadeOrigin::kAuthor);
  cascade.Apply();

  EXPECT_EQ(2.0f, cascade.TakeStyle()->EffectiveZoom());
}

TEST_F(StyleCascadeTest, WritingModeCascadeOrder) {
  TestCascade cascade(GetDocument());
  cascade.Add("writing-mode", "vertical-lr");
  cascade.Add("-webkit-writing-mode", "vertical-rl");
  cascade.Apply();

  EXPECT_EQ("vertical-rl", cascade.ComputedValue("writing-mode"));
  EXPECT_EQ("vertical-rl", cascade.ComputedValue("-webkit-writing-mode"));
}

TEST_F(StyleCascadeTest, WritingModeReversedCascadeOrder) {
  TestCascade cascade(GetDocument());
  cascade.Add("-webkit-writing-mode", "vertical-rl");
  cascade.Add("writing-mode", "vertical-lr");
  cascade.Apply();

  EXPECT_EQ("vertical-lr", cascade.ComputedValue("writing-mode"));
  EXPECT_EQ("vertical-lr", cascade.ComputedValue("-webkit-writing-mode"));
}

TEST_F(StyleCascadeTest, WritingModePriority) {
  TestCascade cascade(GetDocument());
  cascade.Add("writing-mode:vertical-lr !important", Origin::kAuthor);
  cascade.Add("-webkit-writing-mode:vertical-rl", Origin::kAuthor);
  cascade.Apply();

  EXPECT_EQ("vertical-lr", cascade.ComputedValue("writing-mode"));
  EXPECT_EQ("vertical-lr", cascade.ComputedValue("-webkit-writing-mode"));
}

TEST_F(StyleCascadeTest, WebkitBorderImageCascadeOrder) {
  String gradient1("linear-gradient(rgb(0, 0, 0), rgb(0, 128, 0))");
  String gradient2("linear-gradient(rgb(0, 0, 0), rgb(0, 200, 0))");

  TestCascade cascade(GetDocument());
  cascade.Add("-webkit-border-image", gradient1 + " round 40 / 10px / 20px",
              Origin::kAuthor);
  cascade.Add("border-image-source", gradient2, Origin::kAuthor);
  cascade.Add("border-image-slice", "20", Origin::kAuthor);
  cascade.Add("border-image-width", "6px", Origin::kAuthor);
  cascade.Add("border-image-outset", "4px", Origin::kAuthor);
  cascade.Add("border-image-repeat", "space", Origin::kAuthor);
  cascade.Apply();

  EXPECT_EQ(gradient2, cascade.ComputedValue("border-image-source"));
  EXPECT_EQ("20", cascade.ComputedValue("border-image-slice"));
  EXPECT_EQ("6px", cascade.ComputedValue("border-image-width"));
  EXPECT_EQ("4px", cascade.ComputedValue("border-image-outset"));
  EXPECT_EQ("space", cascade.ComputedValue("border-image-repeat"));
}

TEST_F(StyleCascadeTest, WebkitBorderImageReverseCascadeOrder) {
  String gradient1("linear-gradient(rgb(0, 0, 0), rgb(0, 128, 0))");
  String gradient2("linear-gradient(rgb(0, 0, 0), rgb(0, 200, 0))");

  TestCascade cascade(GetDocument());
  cascade.Add("border-image-source", gradient2, Origin::kAuthor);
  cascade.Add("border-image-slice", "20", Origin::kAuthor);
  cascade.Add("border-image-width", "6px", Origin::kAuthor);
  cascade.Add("border-image-outset", "4px", Origin::kAuthor);
  cascade.Add("border-image-repeat", "space", Origin::kAuthor);
  cascade.Add("-webkit-border-image", gradient1 + " round 40 / 10px / 20px",
              Origin::kAuthor);
  cascade.Apply();

  EXPECT_EQ(gradient1, cascade.ComputedValue("border-image-source"));
  EXPECT_EQ("40 fill", cascade.ComputedValue("border-image-slice"));
  EXPECT_EQ("10px", cascade.ComputedValue("border-image-width"));
  EXPECT_EQ("20px", cascade.ComputedValue("border-image-outset"));
  EXPECT_EQ("round", cascade.ComputedValue("border-image-repeat"));
}

TEST_F(StyleCascadeTest, WebkitBorderImageMixedOrder) {
  String gradient1("linear-gradient(rgb(0, 0, 0), rgb(0, 128, 0))");
  String gradient2("linear-gradient(rgb(0, 0, 0), rgb(0, 200, 0))");

  TestCascade cascade(GetDocument());
  cascade.Add("border-image-source", gradient2, Origin::kAuthor);
  cascade.Add("border-image-width", "6px", Origin::kAuthor);
  cascade.Add("-webkit-border-image", gradient1 + " round 40 / 10px / 20px",
              Origin::kAuthor);
  cascade.Add("border-image-slice", "20", Origin::kAuthor);
  cascade.Add("border-image-outset", "4px", Origin::kAuthor);
  cascade.Add("border-image-repeat", "space", Origin::kAuthor);
  cascade.Apply();

  EXPECT_EQ(gradient1, cascade.ComputedValue("border-image-source"));
  EXPECT_EQ("20", cascade.ComputedValue("border-image-slice"));
  EXPECT_EQ("10px", cascade.ComputedValue("border-image-width"));
  EXPECT_EQ("4px", cascade.ComputedValue("border-image-outset"));
  EXPECT_EQ("space", cascade.ComputedValue("border-image-repeat"));
}

TEST_F(StyleCascadeTest, AllLogicalPropertiesSlot) {
  TestCascade cascade(GetDocument());

  static const base::i18n::TextDirection directions[] = {
      base::i18n::TextDirection::LEFT_TO_RIGHT,
      base::i18n::TextDirection::RIGHT_TO_LEFT};

  static const WritingMode modes[] = {WritingMode::kHorizontalTb,
                                      WritingMode::kVerticalRl,
                                      WritingMode::kVerticalLr};

  for (CSSPropertyID id : CSSPropertyIDList()) {
    const CSSProperty& property = CSSProperty::Get(id);

    if (!property.IsLonghand())
      continue;

    for (base::i18n::TextDirection direction : directions) {
      for (WritingMode mode : modes) {
        const CSSProperty& physical =
            property.ResolveDirectionAwareProperty(direction, mode);
        if (&property == &physical)
          continue;

        auto& state = cascade.State();
        state.Style()->SetDirection(direction);
        state.Style()->SetWritingMode(mode);

        // Set logical first.
        {
          StyleCascadeSlots slots;
          EXPECT_TRUE(slots.Set(property, Origin::kAuthor, state));
          EXPECT_FALSE(slots.Set(physical, Origin::kUserAgent, state));
        }

        // Set physical first.
        {
          StyleCascadeSlots slots;
          EXPECT_TRUE(slots.Set(physical, Origin::kAuthor, state));
          EXPECT_FALSE(slots.Set(property, Origin::kUserAgent, state));
        }

        // Set logical twice.
        {
          StyleCascadeSlots slots;
          EXPECT_TRUE(slots.Set(property, Origin::kAuthor, state));
          EXPECT_FALSE(slots.Set(property, Origin::kUserAgent, state));
        }

        // Set physical twice.
        {
          StyleCascadeSlots slots;
          EXPECT_TRUE(slots.Set(physical, Origin::kAuthor, state));
          EXPECT_FALSE(slots.Set(physical, Origin::kUserAgent, state));
        }
      }
    }
  }
}

TEST_F(StyleCascadeTest, MarkReferenced) {
  RegisterProperty(GetDocument(), "--x", "<length>", "0px", false);
  RegisterProperty(GetDocument(), "--y", "<length>", "0px", false);

  TestCascade cascade(GetDocument());
  cascade.Add("width", "var(--x)");
  cascade.Apply();

  const auto* registry = GetDocument().GetPropertyRegistry();
  ASSERT_TRUE(registry);

  EXPECT_TRUE(registry->WasReferenced("--x"));
  EXPECT_FALSE(registry->WasReferenced("--y"));
}

TEST_F(StyleCascadeTest, InternalVisitedColorLonghand) {
  MatchResult result;
  result.FinishAddingUARules();
  result.FinishAddingUserRules();
  result.AddMatchedProperties(ParseDeclarationBlock("color:green"));
  result.AddMatchedProperties(ParseDeclarationBlock("color:red"),
                              CSSSelector::kMatchVisited);
  result.FinishAddingAuthorRulesForTreeScope();

  TestCascade cascade(GetDocument());
  cascade.State().Style()->SetInsideLink(EInsideLink::kInsideVisitedLink);
  cascade.Analyze(result);
  cascade.Apply(result);

  EXPECT_EQ("rgb(0, 128, 0)", cascade.ComputedValue("color"));

  Color red(255, 0, 0);
  const CSSProperty& color = GetCSSPropertyColor();
  EXPECT_EQ(red, cascade.TakeStyle()->VisitedDependentColor(color));
}

TEST_F(StyleCascadeTest, VarInInternalVisitedColorShorthand) {
  MatchResult result;
  result.FinishAddingUARules();
  result.FinishAddingUserRules();
  result.AddMatchedProperties(ParseDeclarationBlock("--x:red"));
  result.AddMatchedProperties(
      ParseDeclarationBlock("outline:medium solid var(--x)"),
      CSSSelector::kMatchVisited);
  result.AddMatchedProperties(ParseDeclarationBlock("outline-color:green"),
                              CSSSelector::kMatchLink);
  result.FinishAddingAuthorRulesForTreeScope();

  TestCascade cascade(GetDocument());
  cascade.State().Style()->SetInsideLink(EInsideLink::kInsideVisitedLink);
  cascade.Analyze(result);
  cascade.Apply(result);

  EXPECT_EQ("rgb(0, 128, 0)", cascade.ComputedValue("outline-color"));

  Color red(255, 0, 0);
  const CSSProperty& outline_color = GetCSSPropertyOutlineColor();
  EXPECT_EQ(red, cascade.TakeStyle()->VisitedDependentColor(outline_color));
}

TEST_F(StyleCascadeTest, ApplyWithFilter) {
  TestCascade cascade(GetDocument());
  cascade.Add("color", "blue", Origin::kAuthor);
  cascade.Add("background-color", "green", Origin::kAuthor);
  cascade.Add("display", "inline", Origin::kAuthor);
  cascade.Apply();
  cascade.Add("color", "green", Origin::kAuthor);
  cascade.Add("background-color", "red", Origin::kAuthor);
  cascade.Add("display", "block", Origin::kAuthor);
  cascade.Apply(CascadeFilter(CSSProperty::kInherited, false));
  EXPECT_EQ("rgb(0, 128, 0)", cascade.ComputedValue("color"));
  EXPECT_EQ("rgb(0, 128, 0)", cascade.ComputedValue("background-color"));
  EXPECT_EQ("inline", cascade.ComputedValue("display"));
}

TEST_F(StyleCascadeTest, UAStyleAbsentWithoutAppearance) {
  TestCascade cascade(GetDocument());
  cascade.Add("background-color", "red", Origin::kUserAgent);
  cascade.Add("border-right-color", "red", Origin::kUserAgent);
  cascade.Apply();
  EXPECT_FALSE(cascade.State().GetUAStyle());
}

TEST_F(StyleCascadeTest, UAStylePresentWithAppearance) {
  TestCascade cascade(GetDocument());
  cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
  cascade.Add("background-color", "red", Origin::kUserAgent);
  cascade.Apply();
  EXPECT_TRUE(cascade.State().GetUAStyle());
}

TEST_F(StyleCascadeTest, UAStyleNoDiffBackground) {
  TestCascade cascade(GetDocument());
  cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
  cascade.Add("background-attachment", "fixed", Origin::kUserAgent);
  cascade.Add("background-blend-mode", "darken", Origin::kUserAgent);
  cascade.Add("background-clip", "padding-box", Origin::kUserAgent);
  cascade.Add("background-image", "none", Origin::kUserAgent);
  cascade.Add("background-origin", "content-box", Origin::kUserAgent);
  cascade.Add("background-position-x", "10px", Origin::kUserAgent);
  cascade.Add("background-position-y", "10px", Origin::kUserAgent);
  cascade.Add("background-size", "contain", Origin::kUserAgent);
  cascade.Apply();
  ASSERT_TRUE(cascade.State().GetUAStyle());
  EXPECT_FALSE(cascade.State().GetUAStyle()->HasDifferentBackground(
      cascade.State().StyleRef()));
}

TEST_F(StyleCascadeTest, UAStyleDiffBackground) {
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("background-image", "linear-gradient(red, green)",
                Origin::kUserAgent);
    cascade.Add("background-attachment", "fixed", Origin::kUserAgent);
    cascade.Add("background-attachment", "unset", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBackground(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("background-image", "linear-gradient(red, green)",
                Origin::kUserAgent);
    cascade.Add("background-blend-mode", "darken", Origin::kUserAgent);
    cascade.Add("background-blend-mode", "unset", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBackground(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("background-image", "linear-gradient(red, green)",
                Origin::kUserAgent);
    cascade.Add("background-clip", "padding-box", Origin::kUserAgent);
    cascade.Add("background-clip", "unset", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBackground(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("background-image", "linear-gradient(green, red)",
                Origin::kUserAgent);
    cascade.Add("background-image", "linear-gradient(red, green)",
                Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBackground(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("background-image", "linear-gradient(green, red)",
                Origin::kUserAgent);
    cascade.Add("background-origin", "content-box", Origin::kUserAgent);
    cascade.Add("background-origin", "unset", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBackground(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("background-image", "linear-gradient(green, red)",
                Origin::kUserAgent);
    cascade.Add("background-position-x", "10px", Origin::kUserAgent);
    cascade.Add("background-position-x", "unset", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBackground(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("background-image", "linear-gradient(green, red)",
                Origin::kUserAgent);
    cascade.Add("background-position-y", "10px", Origin::kUserAgent);
    cascade.Add("background-position-y", "unset", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBackground(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("background-image", "linear-gradient(green, red)",
                Origin::kUserAgent);
    cascade.Add("background-size", "contain", Origin::kUserAgent);
    cascade.Add("background-size", "unset", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBackground(
        cascade.State().StyleRef()));
  }
}

TEST_F(StyleCascadeTest, UAStyleBorderNoDifference) {
  TestCascade cascade(GetDocument());
  cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
  cascade.Add("border-top-color", "red", Origin::kUserAgent);
  cascade.Add("border-right-color", "red", Origin::kUserAgent);
  cascade.Add("border-bottom-color", "red", Origin::kUserAgent);
  cascade.Add("border-left-color", "red", Origin::kUserAgent);
  cascade.Add("border-top-style", "dashed", Origin::kUserAgent);
  cascade.Add("border-right-style", "dashed", Origin::kUserAgent);
  cascade.Add("border-bottom-style", "dashed", Origin::kUserAgent);
  cascade.Add("border-left-style", "dashed", Origin::kUserAgent);
  cascade.Add("border-top-width", "5px", Origin::kUserAgent);
  cascade.Add("border-right-width", "5px", Origin::kUserAgent);
  cascade.Add("border-bottom-width", "5px", Origin::kUserAgent);
  cascade.Add("border-left-width", "5px", Origin::kUserAgent);
  cascade.Add("border-top-left-radius", "5px", Origin::kUserAgent);
  cascade.Add("border-top-right-radius", "5px", Origin::kUserAgent);
  cascade.Add("border-bottom-left-radius", "5px", Origin::kUserAgent);
  cascade.Add("border-bottom-right-radius", "5px", Origin::kUserAgent);
  cascade.Add("border-image-source", "none", Origin::kUserAgent);
  cascade.Add("border-image-slice", "30", Origin::kUserAgent);
  cascade.Add("border-image-width", "10px", Origin::kUserAgent);
  cascade.Add("border-image-outset", "15px", Origin::kUserAgent);
  cascade.Add("border-image-repeat", "round", Origin::kUserAgent);
  cascade.Apply();
  ASSERT_TRUE(cascade.State().GetUAStyle());
  EXPECT_FALSE(cascade.State().GetUAStyle()->HasDifferentBorder(
      cascade.State().StyleRef()));
}

TEST_F(StyleCascadeTest, UAStyleBorderDifference) {
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("border-top-color", "red", Origin::kUserAgent);
    cascade.Add("border-top-color", "green", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBorder(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("border-right-color", "red", Origin::kUserAgent);
    cascade.Add("border-right-color", "green", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBorder(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("border-bottom-color", "red", Origin::kUserAgent);
    cascade.Add("border-bottom-color", "green", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBorder(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("border-left-color", "red", Origin::kUserAgent);
    cascade.Add("border-left-color", "green", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBorder(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("border-top-style", "dashed", Origin::kUserAgent);
    cascade.Add("border-top-style", "solid", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBorder(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("border-right-style", "dashed", Origin::kUserAgent);
    cascade.Add("border-right-style", "solid", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBorder(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("border-bottom-style", "dashed", Origin::kUserAgent);
    cascade.Add("border-bottom-style", "solid", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBorder(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("border-left-style", "dashed", Origin::kUserAgent);
    cascade.Add("border-left-style", "solid", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBorder(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("border-top-style", "solid", Origin::kUserAgent);
    cascade.Add("border-top-width", "10px", Origin::kUserAgent);
    cascade.Add("border-top-width", "9px", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBorder(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("border-right-style", "solid", Origin::kUserAgent);
    cascade.Add("border-right-width", "10px", Origin::kUserAgent);
    cascade.Add("border-right-width", "9px", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBorder(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("border-bottom-style", "solid", Origin::kUserAgent);
    cascade.Add("border-bottom-width", "10px", Origin::kUserAgent);
    cascade.Add("border-bottom-width", "9px", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBorder(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("border-left-style", "solid", Origin::kUserAgent);
    cascade.Add("border-left-width", "10px", Origin::kUserAgent);
    cascade.Add("border-left-width", "9px", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBorder(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("border-top-left-radius", "5px", Origin::kUserAgent);
    cascade.Add("border-top-left-radius", "2px", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBorder(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("border-top-right-radius", "5px", Origin::kUserAgent);
    cascade.Add("border-top-right-radius", "2px", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBorder(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("border-bottom-left-radius", "5px", Origin::kUserAgent);
    cascade.Add("border-bottom-left-radius", "2px", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBorder(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("border-bottom-right-radius", "5px", Origin::kUserAgent);
    cascade.Add("border-bottom-right-radius", "2px", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBorder(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("border-image-source", "none", Origin::kUserAgent);
    cascade.Add("border-image-source", "linear-gradient(red, green)",
                Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBorder(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("border-image-slice", "30", Origin::kUserAgent);
    cascade.Add("border-image-slice", "10", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBorder(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("border-image-width", "10px", Origin::kUserAgent);
    cascade.Add("border-image-width", "15px", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBorder(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("border-image-outset", "10px", Origin::kUserAgent);
    cascade.Add("border-image-outset", "15px", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBorder(
        cascade.State().StyleRef()));
  }
  {
    TestCascade cascade(GetDocument());
    cascade.Add("-webkit-appearance", "button", Origin::kUserAgent);
    cascade.Add("border-image-repeat", "round", Origin::kUserAgent);
    cascade.Add("border-image-repeat", "stretch", Origin::kAuthor);
    cascade.Apply();
    ASSERT_TRUE(cascade.State().GetUAStyle());
    EXPECT_TRUE(cascade.State().GetUAStyle()->HasDifferentBorder(
        cascade.State().StyleRef()));
  }
}

TEST_F(StyleCascadeTest, AnalyzeMatchResult) {
  TestCascade cascade(GetDocument());

  MatchResult result;
  result.AddMatchedProperties(ParseDeclarationBlock("display:none;left:5px"));
  result.AddMatchedProperties(
      ParseDeclarationBlock("font-size:1px !important"));
  result.FinishAddingUARules();
  result.FinishAddingUserRules();
  result.AddMatchedProperties(ParseDeclarationBlock("display:block;color:red"));
  result.AddMatchedProperties(ParseDeclarationBlock("font-size:3px"));
  result.FinishAddingAuthorRulesForTreeScope();

  cascade.Analyze(result);

  auto ua = CascadeOrigin::kUserAgent;
  auto author = CascadeOrigin::kAuthor;

  EXPECT_EQ(cascade.GetPriority("display").GetOrigin(), author);
  EXPECT_EQ(cascade.GetPriority("left").GetOrigin(), ua);
  EXPECT_EQ(cascade.GetPriority("color").GetOrigin(), author);
  EXPECT_EQ(cascade.GetPriority("font-size").GetOrigin(), ua);
}

TEST_F(StyleCascadeTest, AnalyzeMatchResultAll) {
  TestCascade cascade(GetDocument());

  MatchResult result;
  result.AddMatchedProperties(ParseDeclarationBlock("display:block"));
  result.AddMatchedProperties(
      ParseDeclarationBlock("font-size:1px !important"));
  result.FinishAddingUARules();
  result.FinishAddingUserRules();
  result.AddMatchedProperties(ParseDeclarationBlock("all:unset"));
  result.FinishAddingAuthorRulesForTreeScope();

  cascade.Analyze(result);

  auto ua = CascadeOrigin::kUserAgent;
  auto author = CascadeOrigin::kAuthor;

  EXPECT_EQ(cascade.GetPriority("display").GetOrigin(), author);
  EXPECT_EQ(cascade.GetPriority("font-size").GetOrigin(), ua);

  // Random sample from another property affected by 'all'.
  EXPECT_EQ(cascade.GetPriority("color").GetOrigin(), author);
  EXPECT_EQ(cascade.GetPriority("color"), cascade.GetPriority("display"));
}

TEST_F(StyleCascadeTest, AnalyzeMatchResultFilter) {
  TestCascade cascade(GetDocument());

  MatchResult result;
  result.FinishAddingUARules();
  result.FinishAddingUserRules();
  result.AddMatchedProperties(ParseDeclarationBlock("display:block"));
  result.AddMatchedProperties(ParseDeclarationBlock("color:red"));
  result.AddMatchedProperties(ParseDeclarationBlock("font-size:3px"));
  result.FinishAddingAuthorRulesForTreeScope();

  cascade.Analyze(result, CascadeFilter(CSSProperty::kInherited, true));

  auto none = CascadeOrigin::kNone;
  auto author = CascadeOrigin::kAuthor;

  EXPECT_EQ(cascade.GetPriority("display").GetOrigin(), author);
  EXPECT_EQ(cascade.GetPriority("color").GetOrigin(), none);
  EXPECT_EQ(cascade.GetPriority("font-size").GetOrigin(), none);
}

TEST_F(StyleCascadeTest, AnalyzeMatchResultAllFilter) {
  TestCascade cascade(GetDocument());

  MatchResult result;
  result.FinishAddingUARules();
  result.FinishAddingUserRules();
  result.AddMatchedProperties(ParseDeclarationBlock("all:unset"));
  result.FinishAddingAuthorRulesForTreeScope();

  cascade.Analyze(result, CascadeFilter(CSSProperty::kInherited, true));

  auto none = CascadeOrigin::kNone;
  auto author = CascadeOrigin::kAuthor;

  EXPECT_EQ(cascade.GetPriority("display").GetOrigin(), author);
  EXPECT_EQ(cascade.GetPriority("color").GetOrigin(), none);
  EXPECT_EQ(cascade.GetPriority("font-size").GetOrigin(), none);
}

TEST_F(StyleCascadeTest, MarkHasReferenceLonghand) {
  TestCascade cascade(GetDocument());

  cascade.Add("--x:red");
  cascade.Add("background-color:var(--x)");
  cascade.Apply();

  EXPECT_TRUE(cascade.State()
                  .StyleRef()
                  .HasVariableReferenceFromNonInheritedProperty());
}

TEST_F(StyleCascadeTest, MarkHasReferenceShorthand) {
  TestCascade cascade(GetDocument());

  cascade.Add("--x:red");
  cascade.Add("background:var(--x)");
  cascade.Apply();

  EXPECT_TRUE(cascade.State()
                  .StyleRef()
                  .HasVariableReferenceFromNonInheritedProperty());
}

TEST_F(StyleCascadeTest, NoMarkHasReferenceForInherited) {
  TestCascade cascade(GetDocument());

  cascade.Add("--x:red");
  cascade.Add("--y:caption");
  cascade.Add("color:var(--x)");
  cascade.Add("font:var(--y)");
  cascade.Apply();

  EXPECT_FALSE(cascade.State()
                   .StyleRef()
                   .HasVariableReferenceFromNonInheritedProperty());
}

// TODO: Check that tests don't crash when passing wrong match_result/
// interpolations to Apply.

}  // namespace blink
