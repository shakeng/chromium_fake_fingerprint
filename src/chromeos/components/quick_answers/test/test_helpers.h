// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEOS_COMPONENTS_QUICK_ANSWERS_TEST_TEST_HELPERS_H_
#define CHROMEOS_COMPONENTS_QUICK_ANSWERS_TEST_TEST_HELPERS_H_

#include "chromeos/components/quick_answers/quick_answers_client.h"
#include "chromeos/components/quick_answers/quick_answers_model.h"
#include "testing/gmock/include/gmock/gmock.h"

namespace chromeos {
namespace quick_answers {

class MockQuickAnswersDelegate
    : public QuickAnswersClient::QuickAnswersDelegate {
 public:
  MockQuickAnswersDelegate();
  ~MockQuickAnswersDelegate() override;

  MockQuickAnswersDelegate(const MockQuickAnswersDelegate&) = delete;
  MockQuickAnswersDelegate& operator=(const MockQuickAnswersDelegate&) = delete;

  // QuickAnswersClient::QuickAnswersDelegate:
  MOCK_METHOD1(OnQuickAnswerReceived, void(std::unique_ptr<QuickAnswer>));
  MOCK_METHOD1(OnRequestPreprocessFinish, void(const QuickAnswersRequest&));
  MOCK_METHOD1(OnEligibilityChanged, void(bool));
  MOCK_METHOD0(OnNetworkError, void());
};

MATCHER_P(QuickAnswerEqual, quick_answer, "") {
  return (arg->primary_answer == quick_answer->primary_answer);
}

MATCHER_P(QuickAnswersRequestEqual, quick_answers_request, "") {
  return (arg.selected_text == quick_answers_request.selected_text);
}

}  // namespace quick_answers
}  // namespace chromeos

#endif  // CHROMEOS_COMPONENTS_QUICK_ANSWERS_TEST_TEST_HELPERS_H_
