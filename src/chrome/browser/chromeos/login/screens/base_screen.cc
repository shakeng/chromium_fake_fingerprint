// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/login/screens/base_screen.h"

#include "base/logging.h"

namespace chromeos {

BaseScreen::BaseScreen(OobeScreenId screen_id) : screen_id_(screen_id) {}

BaseScreen::~BaseScreen() {}

void BaseScreen::Show() {
  ShowImpl();
  is_hidden_ = false;
}

void BaseScreen::Hide() {
  HideImpl();
  is_hidden_ = true;
}

void BaseScreen::HandleUserAction(const std::string& action_id) {
  if (is_hidden_) {
    LOG(WARNING) << "User action came when screen is hidden: action_id="
                 << action_id;
    return;
  }
  OnUserAction(action_id);
}

void BaseScreen::OnUserAction(const std::string& action_id) {
  LOG(WARNING) << "Unhandled user action: action_id=" << action_id;
}

void BaseScreen::SetConfiguration(base::Value* configuration) {
  configuration_ = configuration;
}

}  // namespace chromeos
