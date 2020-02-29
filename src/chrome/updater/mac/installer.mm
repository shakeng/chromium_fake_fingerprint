// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <Cocoa/Cocoa.h>

#include <string>
#include <vector>

#include "chrome/updater/mac/installer.h"

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/mac/scoped_nsobject.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/strings/sys_string_conversions.h"

namespace updater {

namespace {

bool RunHDIUtil(const std::vector<std::string>& args,
                std::string* command_output) {
  base::FilePath hdiutil_path("/usr/bin/hdiutil");
  if (!base::PathExists(hdiutil_path)) {
    DLOG(ERROR) << "hdiutil path (" << hdiutil_path << ") does not exist.";
    return false;
  }

  base::CommandLine command(hdiutil_path);
  for (const auto& arg : args)
    command.AppendArg(arg);

  std::string output;
  bool result = base::GetAppOutput(command, &output);
  if (!result)
    DLOG(ERROR) << "hdiutil failed.";

  if (command_output)
    *command_output = output;

  return result;
}

bool MountDMG(const base::FilePath& dmg_path, std::string* mount_point) {
  if (!base::PathExists(dmg_path)) {
    DLOG(ERROR) << "The DMG file path (" << dmg_path << ") does not exist.";
    return false;
  }

  std::string command_output;
  std::vector<std::string> args{"attach", dmg_path.value(), "-plist",
                                "-nobrowse", "-readonly"};
  if (!RunHDIUtil(args, &command_output)) {
    DLOG(ERROR) << "Mounting DMG (" << dmg_path
                << ") failed. Output: " << command_output;
    return false;
  }
  @autoreleasepool {
    NSString* output = base::SysUTF8ToNSString(command_output);
    NSDictionary* plist = [output propertyList];
    // Look for the mountpoint.
    NSArray* system_entities = [plist objectForKey:@"system-entities"];
    NSString* dmg_mount_point = nil;
    for (NSDictionary* entry in system_entities) {
      NSString* entry_mount_point = entry[@"mount-point"];
      if ([entry_mount_point length]) {
        dmg_mount_point = [entry_mount_point stringByStandardizingPath];
        break;
      }
    }
    if (mount_point)
      *mount_point = base::SysNSStringToUTF8(dmg_mount_point);
  }
  return true;
}

bool UnmountDMG(const base::FilePath& mounted_dmg_path) {
  if (!base::PathExists(mounted_dmg_path)) {
    DLOG(ERROR) << "The mounted DMG path (" << mounted_dmg_path
                << ") does not exist.";
    return false;
  }

  std::vector<std::string> args{"detach", mounted_dmg_path.value(), "-force"};
  if (!RunHDIUtil(args, nullptr)) {
    DLOG(ERROR) << "Unmounting DMG (" << mounted_dmg_path << ") failed.";
    return false;
  }
  return true;
}

bool RunScript(const base::FilePath& mounted_dmg_path,
               const base::FilePath::StringPieceType script_name) {
  if (!base::PathExists(mounted_dmg_path)) {
    DLOG(ERROR) << "File path (" << mounted_dmg_path << ") does not exist.";
    return false;
  }
  const base::FilePath script_file_path = mounted_dmg_path.Append(script_name);
  if (!base::PathExists(script_file_path)) {
    DLOG(ERROR) << "Script file path (" << script_file_path
                << ") does not exist.";
    return false;
  }

  base::CommandLine command(script_file_path);

  std::string output;
  if (!base::GetAppOutput(command, &output)) {
    DLOG(ERROR) << "Installer script failed.";
    return false;
  }

  return true;
}

}  // namespace

bool InstallFromDMG(const base::FilePath& dmg_file_path) {
  std::string mount_point;
  if (!MountDMG(dmg_file_path, &mount_point))
    return false;

  if (mount_point.empty()) {
    DLOG(ERROR) << "No mount point.";
    return false;
  }

  const base::FilePath mounted_dmg_path = base::FilePath(mount_point);
  bool result = RunScript(mounted_dmg_path, ".install.sh");

  if (!UnmountDMG(mounted_dmg_path))
    DLOG(WARNING) << "Could not unmount the DMG: " << mounted_dmg_path;

  return result;
}

}  // namespace updater
