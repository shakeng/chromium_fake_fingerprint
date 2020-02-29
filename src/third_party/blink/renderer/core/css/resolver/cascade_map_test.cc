// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/resolver/cascade_map.h"
#include <gtest/gtest.h>
#include "third_party/blink/renderer/core/css/css_property_name.h"
#include "third_party/blink/renderer/core/css/css_property_names.h"
#include "third_party/blink/renderer/core/css/resolver/cascade_priority.h"
#include "third_party/blink/renderer/core/css/resolver/css_property_priority.h"

namespace blink {

TEST(CascadeMapTest, Empty) {
  CascadeMap map;
  EXPECT_FALSE(map.Find(CSSPropertyName(AtomicString("--x"))));
  EXPECT_FALSE(map.Find(CSSPropertyName(AtomicString("--y"))));
  EXPECT_FALSE(map.Find(CSSPropertyName(CSSPropertyID::kColor)));
  EXPECT_FALSE(map.Find(CSSPropertyName(CSSPropertyID::kDisplay)));
}

TEST(CascadeMapTest, AddCustom) {
  CascadeMap map;
  CascadePriority ua(CascadeOrigin::kUserAgent);
  CascadePriority author(CascadeOrigin::kAuthor);
  CSSPropertyName x(AtomicString("--x"));
  CSSPropertyName y(AtomicString("--y"));

  EXPECT_TRUE(map.Add(x, ua));
  EXPECT_TRUE(map.Add(x, author));
  EXPECT_FALSE(map.Add(x, author));
  ASSERT_TRUE(map.Find(x));
  EXPECT_EQ(author, *map.Find(x));

  EXPECT_FALSE(map.Find(y));
  EXPECT_TRUE(map.Add(y, ua));

  // --x should be unchanged.
  ASSERT_TRUE(map.Find(x));
  EXPECT_EQ(author, *map.Find(x));

  // --y should exist too.
  ASSERT_TRUE(map.Find(y));
  EXPECT_EQ(ua, *map.Find(y));
}

TEST(CascadeMapTest, AddNative) {
  CascadeMap map;
  CascadePriority ua(CascadeOrigin::kUserAgent);
  CascadePriority author(CascadeOrigin::kAuthor);
  CSSPropertyName color(CSSPropertyID::kColor);
  CSSPropertyName display(CSSPropertyID::kDisplay);

  EXPECT_TRUE(map.Add(color, ua));
  EXPECT_TRUE(map.Add(color, author));
  EXPECT_FALSE(map.Add(color, author));
  ASSERT_TRUE(map.Find(color));
  EXPECT_EQ(author, *map.Find(color));

  EXPECT_FALSE(map.Find(display));
  EXPECT_TRUE(map.Add(display, ua));

  // color should be unchanged.
  ASSERT_TRUE(map.Find(color));
  EXPECT_EQ(author, *map.Find(color));

  // display should exist too.
  ASSERT_TRUE(map.Find(display));
  EXPECT_EQ(ua, *map.Find(display));
}

TEST(CascadeMapTest, FindAndMutateCustom) {
  CascadeMap map;
  CascadePriority ua(CascadeOrigin::kUserAgent);
  CascadePriority author(CascadeOrigin::kAuthor);
  CSSPropertyName x(AtomicString("--x"));

  EXPECT_TRUE(map.Add(x, ua));

  CascadePriority* p = map.Find(x);
  ASSERT_TRUE(p);
  EXPECT_EQ(ua, *p);

  *p = author;

  EXPECT_FALSE(map.Add(x, author));
  ASSERT_TRUE(map.Find(x));
  EXPECT_EQ(author, *map.Find(x));
}

TEST(CascadeMapTest, FindAndMutateNative) {
  CascadeMap map;
  CascadePriority ua(CascadeOrigin::kUserAgent);
  CascadePriority author(CascadeOrigin::kAuthor);
  CSSPropertyName color(CSSPropertyID::kColor);

  EXPECT_TRUE(map.Add(color, ua));

  CascadePriority* p = map.Find(color);
  ASSERT_TRUE(p);
  EXPECT_EQ(ua, *p);

  *p = author;

  EXPECT_FALSE(map.Add(color, author));
  ASSERT_TRUE(map.Find(color));
  EXPECT_EQ(author, *map.Find(color));
}

TEST(CascadeMapTest, AtCustom) {
  CascadeMap map;
  CascadePriority ua(CascadeOrigin::kUserAgent);
  CascadePriority author(CascadeOrigin::kAuthor);
  CSSPropertyName x(AtomicString("--x"));

  EXPECT_EQ(CascadePriority(), map.At(x));

  EXPECT_TRUE(map.Add(x, ua));
  EXPECT_EQ(ua, map.At(x));

  EXPECT_TRUE(map.Add(x, author));
  EXPECT_EQ(author, map.At(x));
}

TEST(CascadeMapTest, AtNative) {
  CascadeMap map;
  CascadePriority ua(CascadeOrigin::kUserAgent);
  CascadePriority author(CascadeOrigin::kAuthor);
  CSSPropertyName color(CSSPropertyID::kColor);

  EXPECT_EQ(CascadePriority(), map.At(color));

  EXPECT_TRUE(map.Add(color, ua));
  EXPECT_EQ(ua, map.At(color));

  EXPECT_TRUE(map.Add(color, author));
  EXPECT_EQ(author, map.At(color));
}

TEST(CascadeMapTest, HighPriorityBits) {
  CascadeMap map;

  EXPECT_FALSE(map.HighPriorityBits());

  map.Add(CSSPropertyName(CSSPropertyID::kFontSize), CascadeOrigin::kAuthor);
  EXPECT_EQ(map.HighPriorityBits(),
            1ull << static_cast<uint64_t>(CSSPropertyID::kFontSize));

  map.Add(CSSPropertyName(CSSPropertyID::kColor), CascadeOrigin::kAuthor);
  map.Add(CSSPropertyName(CSSPropertyID::kFontSize), CascadeOrigin::kAuthor);
  EXPECT_EQ(map.HighPriorityBits(),
            (1ull << static_cast<uint64_t>(CSSPropertyID::kFontSize)) |
                (1ull << static_cast<uint64_t>(CSSPropertyID::kColor)));
}

TEST(CascadeMapTest, AllHighPriorityBits) {
  CascadeMap map;

  EXPECT_FALSE(map.HighPriorityBits());

  uint64_t expected = 0;
  for (CSSPropertyID id : CSSPropertyIDList()) {
    if (CSSPropertyPriorityData<kHighPropertyPriority>::PropertyHasPriority(
            id)) {
      map.Add(CSSPropertyName(id), CascadeOrigin::kAuthor);
      expected |= (1ull << static_cast<uint64_t>(id));
    }
  }

  EXPECT_EQ(expected, map.HighPriorityBits());
}

TEST(CascadeMapTest, LastHighPrio) {
  CascadeMap map;

  EXPECT_FALSE(map.HighPriorityBits());

  CSSPropertyID last = CSSPropertyPriorityData<kHighPropertyPriority>::Last();

  map.Add(CSSPropertyName(last), CascadeOrigin::kAuthor);
  EXPECT_EQ(map.HighPriorityBits(), 1ull << static_cast<uint64_t>(last));
}

}  // namespace blink
