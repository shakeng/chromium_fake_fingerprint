load('//lib/builders.star', 'cpu', 'goma', 'os')
load('//lib/ci.star', 'ci')
load('//versioned/vars/ci.star', 'vars')
# Load this using relative path so that the load statement doesn't
# need to be changed when making a new milestone
load('../vars.star', milestone_vars='vars')

luci.bucket(
    name = vars.bucket.get(),
    acls = [
        acl.entry(
            roles = acl.BUILDBUCKET_READER,
            groups = 'all',
        ),
        acl.entry(
            roles = acl.BUILDBUCKET_TRIGGERER,
            groups = 'project-chromium-ci-schedulers',
        ),
        acl.entry(
            roles = acl.BUILDBUCKET_OWNER,
            groups = 'google/luci-task-force@google.com',
        ),
    ],
)

luci.gitiles_poller(
    name = vars.poller.get(),
    bucket = vars.bucket.get(),
    repo = 'https://chromium.googlesource.com/chromium/src',
    refs = [milestone_vars.ref],
)


ci.defaults.bucket.set(vars.bucket.get())
ci.defaults.bucketed_triggers.set(True)
ci.defaults.triggered_by.set([vars.poller.get()])


# Builders are sorted first lexicographically by the function used to define
# them, then lexicographically by their name


ci.android_builder(
    name = 'Android WebView L (dbg)',
    triggered_by = [vars.bucket.builder('Android arm Builder (dbg)')],
)

ci.android_builder(
    name = 'Android arm Builder (dbg)',
    execution_timeout = 4 * time.hour,
)

ci.android_builder(
    name = 'Cast Android (dbg)',
)

ci.android_builder(
    name = 'KitKat Phone Tester (dbg)',
    triggered_by = [vars.bucket.builder('Android arm Builder (dbg)')],
)

ci.android_builder(
    name = 'KitKat Tablet Tester',
    # We have limited tablet capacity and thus limited ability to run
    # tests in parallel, hence the high timeout.
    execution_timeout = 20 * time.hour,
    triggered_by = [vars.bucket.builder('Android arm Builder (dbg)')],
)

ci.android_builder(
    name = 'Lollipop Phone Tester',
    # We have limited phone capacity and thus limited ability to run
    # tests in parallel, hence the high timeout.
    execution_timeout = 6 * time.hour,
    triggered_by = [vars.bucket.builder('Android arm Builder (dbg)')],
)

ci.android_builder(
    name = 'Lollipop Tablet Tester',
    # We have limited tablet capacity and thus limited ability to run
    # tests in parallel, hence the high timeout.
    execution_timeout = 20 * time.hour,
    triggered_by = [vars.bucket.builder('Android arm Builder (dbg)')],
)

ci.android_builder(
    name = 'Marshmallow Tablet Tester',
    # We have limited tablet capacity and thus limited ability to run
    # tests in parallel, hence the high timeout.
    execution_timeout = 12 * time.hour,
    triggered_by = [vars.bucket.builder('Android arm Builder (dbg)')],
)

ci.android_builder(
    name = 'android-cronet-arm-rel',
    notifies = ['cronet'],
)

ci.android_builder(
    name = 'android-cronet-kitkat-arm-rel',
    notifies = ['cronet'],
    triggered_by = [vars.bucket.builder('android-cronet-arm-rel')],
)

ci.android_builder(
    name = 'android-cronet-lollipop-arm-rel',
    notifies = ['cronet'],
    triggered_by = [vars.bucket.builder('android-cronet-arm-rel')],
)

ci.android_builder(
    name = 'android-kitkat-arm-rel',
)

ci.android_builder(
    name = 'android-marshmallow-arm64-rel',
)


ci.chromiumos_builder(
    name = 'chromeos-amd64-generic-rel',
)

ci.chromiumos_builder(
    name = 'chromeos-arm-generic-rel',
)

ci.chromiumos_builder(
    name = 'linux-chromeos-dbg',
)

ci.chromiumos_builder(
    name = 'linux-chromeos-rel',
)


# This is launching & collecting entirely isolated tests.
# OS shouldn't matter.
ci.fyi_builder(
    name = 'mac-osxbeta-rel',
    goma_backend = None,
    triggered_by = [vars.bucket.builder('Mac Builder')],
)


ci.fyi_windows_builder(
    name = 'Win10 Tests x64 1803',
    goma_backend = None,
    os = os.WINDOWS_10,
    triggered_by = [vars.bucket.builder('Win x64 Builder')],
)


ci.gpu_builder(
    name = 'Android Release (Nexus 5X)',
)

ci.gpu_builder(
    name = 'GPU Linux Builder',
)

ci.gpu_builder(
    name = 'GPU Mac Builder',
    cores = None,
    os = os.MAC_ANY,
)

ci.gpu_builder(
    name = 'GPU Win x64 Builder',
    builderless = True,
    os = os.WINDOWS_ANY,
)


ci.gpu_thin_tester(
    name = 'Linux Release (NVIDIA)',
    triggered_by = [vars.bucket.builder('GPU Linux Builder')],
)

ci.gpu_thin_tester(
    name = 'Mac Release (Intel)',
    triggered_by = [vars.bucket.builder('GPU Mac Builder')],
)

ci.gpu_thin_tester(
    name = 'Mac Retina Release (AMD)',
    triggered_by = [vars.bucket.builder('GPU Mac Builder')],
)

ci.gpu_thin_tester(
    name = 'Win10 x64 Release (NVIDIA)',
    triggered_by = [vars.bucket.builder('GPU Win x64 Builder')],
)


ci.linux_builder(
    name = 'Cast Linux',
    goma_jobs = goma.jobs.J50,
)

ci.linux_builder(
    name = 'Fuchsia ARM64',
    notifies = ['cr-fuchsia'],
)

ci.linux_builder(
    name = 'Fuchsia x64',
    notifies = ['cr-fuchsia'],
)

ci.linux_builder(
    name = 'Linux Builder',
)

ci.linux_builder(
    name = 'Linux Tests',
    goma_backend = None,
    triggered_by = [vars.bucket.builder('Linux Builder')],
)

ci.linux_builder(
    name = 'linux-ozone-rel',
)

ci.linux_builder(
    name = 'Linux Ozone Tester (Wayland)',
    triggered_by = [vars.bucket.builder('linux-ozone-rel')],
)

ci.linux_builder(
    name = 'Linux Ozone Tester (X11)',
    triggered_by = [vars.bucket.builder('linux-ozone-rel')],
)


ci.mac_builder(
    name = 'Mac Builder',
)

ci.mac_builder(
    name = 'Mac Builder (dbg)',
    os = os.MAC_ANY,
)

# The build runs on 10.13, but triggers tests on 10.10 bots.
ci.mac_builder(
    name = 'Mac10.10 Tests',
    triggered_by = [vars.bucket.builder('Mac Builder')],
)

# The build runs on 10.13, but triggers tests on 10.11 bots.
ci.mac_builder(
    name = 'Mac10.11 Tests',
    triggered_by = [vars.bucket.builder('Mac Builder')],
)

ci.mac_builder(
    name = 'Mac10.12 Tests',
    os = os.MAC_10_12,
    triggered_by = [vars.bucket.builder('Mac Builder')],
)

ci.mac_builder(
    name = 'Mac10.13 Tests',
    os = os.MAC_10_13,
    triggered_by = [vars.bucket.builder('Mac Builder')],
)

ci.mac_builder(
    name = 'Mac10.13 Tests (dbg)',
    os = os.MAC_ANY,
    triggered_by = [vars.bucket.builder('Mac Builder (dbg)')],
)

ci.mac_builder(
    name = 'WebKit Mac10.13 (retina)',
    os = os.MAC_10_13,
    triggered_by = [vars.bucket.builder('Mac Builder')],
)


ci.mac_ios_builder(
    name = 'ios-simulator',
)


ci.memory_builder(
    name = 'Linux ASan LSan Builder',
    ssd = True,
)

ci.memory_builder(
    name = 'Linux ASan LSan Tests (1)',
    triggered_by = [vars.bucket.builder('Linux ASan LSan Builder')],
)

ci.memory_builder(
    name = 'Linux ASan Tests (sandboxed)',
    triggered_by = [vars.bucket.builder('Linux ASan LSan Builder')],
)


ci.win_builder(
    name = 'Win7 Tests (dbg)(1)',
    os = os.WINDOWS_7,
    triggered_by = [vars.bucket.builder('Win Builder (dbg)')],
)

ci.win_builder(
    name = 'Win 7 Tests x64 (1)',
    os = os.WINDOWS_7,
    triggered_by = [vars.bucket.builder('Win x64 Builder')],
)

ci.win_builder(
    name = 'Win Builder (dbg)',
    cores = 32,
    os = os.WINDOWS_ANY,
)

ci.win_builder(
    name = 'Win x64 Builder',
    cores = 32,
    os = os.WINDOWS_ANY,
)

ci.win_builder(
    name = 'Win10 Tests x64',
    triggered_by = [vars.bucket.builder('Win x64 Builder')],
)
