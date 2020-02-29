// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.photo_picker;

import android.content.Context;
import android.graphics.Bitmap;
import android.net.Uri;
import android.os.Build;
import android.support.test.filters.LargeTest;

import org.junit.Assert;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;

import org.chromium.base.test.util.CallbackHelper;
import org.chromium.base.test.util.CommandLineFlags;
import org.chromium.base.test.util.DisabledTest;
import org.chromium.base.test.util.MinAndroidSdkLevel;
import org.chromium.base.test.util.UrlUtils;
import org.chromium.chrome.browser.ChromeActivity;
import org.chromium.chrome.browser.ChromeSwitches;
import org.chromium.chrome.test.ChromeActivityTestRule;
import org.chromium.chrome.test.ChromeJUnit4ClassRunner;

import java.io.File;
import java.util.List;
import java.util.concurrent.TimeUnit;

/**
 * Tests for the DecoderServiceHost.
 */
@RunWith(ChromeJUnit4ClassRunner.class)
@MinAndroidSdkLevel(Build.VERSION_CODES.N)
@CommandLineFlags.Add({ChromeSwitches.DISABLE_FIRST_RUN_EXPERIENCE})
public class DecoderServiceHostTest implements DecoderServiceHost.ServiceReadyCallback,
                                               DecoderServiceHost.ImagesDecodedCallback {
    // The timeout (in seconds) to wait for the decoding.
    private static final long WAIT_TIMEOUT_SECONDS = 5L;

    @Rule
    public ChromeActivityTestRule<ChromeActivity> mActivityTestRule =
            new ChromeActivityTestRule<>(ChromeActivity.class);

    private Context mContext;

    // A callback that fires when the decoder is ready.
    public final CallbackHelper onDecoderReadyCallback = new CallbackHelper();

    // A callback that fires when something is finished decoding in the dialog.
    public final CallbackHelper onDecodedCallback = new CallbackHelper();

    private String mLastDecodedPath;
    private boolean mLastIsVideo;
    private Bitmap mLastInitialFrame;
    private int mLastFrameCount;
    private String mLastVideoDuration;
    private float mLastRatio;

    @Before
    public void setUp() throws Exception {
        mActivityTestRule.startMainActivityOnBlankPage();
        mContext = mActivityTestRule.getActivity();

        DecoderServiceHost.setReadyCallback(this);
    }

    // DecoderServiceHost.ServiceReadyCallback:

    @Override
    public void serviceReady() {
        onDecoderReadyCallback.notifyCalled();
    }

    // DecoderServiceHost.ImagesDecodedCallback:

    @Override
    public void imagesDecodedCallback(String filePath, boolean isVideo, boolean isZoomedIn,
            List<Bitmap> bitmaps, String videoDuration, float ratio) {
        mLastDecodedPath = filePath;
        mLastIsVideo = isVideo;
        mLastFrameCount = bitmaps != null ? bitmaps.size() : -1;
        mLastInitialFrame = bitmaps != null ? bitmaps.get(0) : null;
        mLastVideoDuration = videoDuration;
        mLastRatio = ratio;

        onDecodedCallback.notifyCalled();
    }

    private void waitForDecoder() throws Exception {
        int callCount = onDecoderReadyCallback.getCallCount();
        onDecoderReadyCallback.waitForCallback(
                callCount, 1, WAIT_TIMEOUT_SECONDS, TimeUnit.SECONDS);
    }

    private void waitForThumbnailDecode() throws Exception {
        int callCount = onDecodedCallback.getCallCount();
        onDecodedCallback.waitForCallback(callCount, 1, WAIT_TIMEOUT_SECONDS, TimeUnit.SECONDS);
    }

    @Test
    @LargeTest
    public void testDecodingOrder() throws Throwable {
        DecoderServiceHost host = new DecoderServiceHost(this, mContext);
        host.bind(mContext);
        waitForDecoder();

        String video1 = "noogler.mp4";
        String video2 = "noogler2.mp4";
        String jpg1 = "blue100x100.jpg";
        String filePath = "chrome/test/data/android/photo_picker/";
        File file1 = new File(UrlUtils.getIsolatedTestFilePath(filePath + video1));
        File file2 = new File(UrlUtils.getIsolatedTestFilePath(filePath + video2));
        File file3 = new File(UrlUtils.getIsolatedTestFilePath(filePath + jpg1));

        host.decodeImage(
                Uri.fromFile(file1), PickerBitmap.TileTypes.VIDEO, 10, /*fullWidth=*/false, this);
        host.decodeImage(
                Uri.fromFile(file2), PickerBitmap.TileTypes.VIDEO, 10, /*fullWidth=*/false, this);
        host.decodeImage(
                Uri.fromFile(file3), PickerBitmap.TileTypes.PICTURE, 10, /*fullWidth=*/false, this);

        // First decoding result should be first frame of video 1. Even though still images take
        // priority over video decoding, video 1 will be the only item in the queue when the first
        // decoding request is kicked off (as a result of calling decodeImage).
        waitForThumbnailDecode();
        Assert.assertTrue(mLastDecodedPath.contains(video1));
        Assert.assertEquals(true, mLastIsVideo);
        Assert.assertEquals("0:00", mLastVideoDuration);
        Assert.assertEquals(1, mLastFrameCount);

        // When the decoder is finished with the first frame of video 1, there will be two new
        // requests available for processing. Video2 was added first, but that will be skipped in
        // favor of the still image, so the jpg is expected to be decoded next.
        waitForThumbnailDecode();
        Assert.assertTrue(mLastDecodedPath.contains(jpg1));
        Assert.assertEquals(false, mLastIsVideo);
        Assert.assertEquals(null, mLastVideoDuration);
        Assert.assertEquals(1, mLastFrameCount);

        // Third decoding result is first frame of video 2, because that's higher priority than the
        // rest of video 1.
        waitForThumbnailDecode();
        Assert.assertTrue(mLastDecodedPath.contains(video2));
        Assert.assertEquals(true, mLastIsVideo);
        Assert.assertEquals("0:00", mLastVideoDuration);
        Assert.assertEquals(1, mLastFrameCount);

        // Remaining frames of video 1.
        waitForThumbnailDecode();
        Assert.assertTrue(mLastDecodedPath.contains(video1));
        Assert.assertEquals(true, mLastIsVideo);
        Assert.assertEquals("0:00", mLastVideoDuration);
        Assert.assertEquals(10, mLastFrameCount);

        // Remaining frames of video 2.
        waitForThumbnailDecode();
        Assert.assertTrue(mLastDecodedPath.contains(video2));
        Assert.assertEquals(true, mLastIsVideo);
        Assert.assertEquals("0:00", mLastVideoDuration);
        Assert.assertEquals(10, mLastFrameCount);

        host.unbind(mContext);
    }

    @Test
    @LargeTest
    public void testDecodingSizes() throws Throwable {
        DecoderServiceHost host = new DecoderServiceHost(this, mContext);
        host.bind(mContext);
        waitForDecoder();

        String video1 = "noogler.mp4"; // 1920 x 1080 video.
        String jpg1 = "blue100x100.jpg";
        String filePath = "chrome/test/data/android/photo_picker/";
        File file1 = new File(UrlUtils.getIsolatedTestFilePath(filePath + video1));
        File file2 = new File(UrlUtils.getIsolatedTestFilePath(filePath + jpg1));

        // Thumbnail photo. 100 x 100 -> 10 x 10.
        host.decodeImage(
                Uri.fromFile(file2), PickerBitmap.TileTypes.PICTURE, 10, /*fullWidth=*/false, this);
        waitForThumbnailDecode();
        Assert.assertTrue(mLastDecodedPath.contains(jpg1));
        Assert.assertEquals(false, mLastIsVideo);
        Assert.assertEquals(null, mLastVideoDuration);
        Assert.assertEquals(1, mLastFrameCount);
        Assert.assertEquals(1.0f, mLastRatio, 0.1f);
        Assert.assertEquals(10, mLastInitialFrame.getWidth());
        Assert.assertEquals(10, mLastInitialFrame.getHeight());

        // Full-width photo. 100 x 100 -> 200 x 200.
        host.decodeImage(
                Uri.fromFile(file2), PickerBitmap.TileTypes.PICTURE, 200, /*fullWidth=*/true, this);
        waitForThumbnailDecode();
        Assert.assertTrue(mLastDecodedPath.contains(jpg1));
        Assert.assertEquals(false, mLastIsVideo);
        Assert.assertEquals(null, mLastVideoDuration);
        Assert.assertEquals(1, mLastFrameCount);
        Assert.assertEquals(1.0f, mLastRatio, 0.1f);
        Assert.assertEquals(200, mLastInitialFrame.getWidth());
        Assert.assertEquals(200, mLastInitialFrame.getHeight());

        // Thumbnail video. 1920 x 1080 -> 10 x 10.
        host.decodeImage(
                Uri.fromFile(file1), PickerBitmap.TileTypes.VIDEO, 10, /*fullWidth=*/false, this);
        waitForThumbnailDecode(); // Initial frame.
        Assert.assertTrue(mLastDecodedPath.contains(video1));
        Assert.assertEquals(true, mLastIsVideo);
        Assert.assertEquals("0:00", mLastVideoDuration);
        Assert.assertEquals(1, mLastFrameCount);
        Assert.assertEquals(0.5625f, mLastRatio, 0.0001f);
        Assert.assertEquals(10, mLastInitialFrame.getWidth());
        Assert.assertEquals(10, mLastInitialFrame.getHeight());
        waitForThumbnailDecode(); // Rest of frames.
        Assert.assertTrue(mLastDecodedPath.contains(video1));
        Assert.assertEquals(true, mLastIsVideo);
        Assert.assertEquals("0:00", mLastVideoDuration);
        Assert.assertEquals(10, mLastFrameCount);
        Assert.assertEquals(0.5625f, mLastRatio, 0.0001f);
        Assert.assertEquals(10, mLastInitialFrame.getWidth());
        Assert.assertEquals(10, mLastInitialFrame.getHeight());

        // Full-width video. 1920 x 1080 -> 2000 x 1125.
        host.decodeImage(
                Uri.fromFile(file1), PickerBitmap.TileTypes.VIDEO, 2000, /*fullWidth=*/true, this);
        waitForThumbnailDecode(); // Initial frame.
        Assert.assertTrue(mLastDecodedPath.contains(video1));
        Assert.assertEquals(true, mLastIsVideo);
        Assert.assertEquals("0:00", mLastVideoDuration);
        Assert.assertEquals(1, mLastFrameCount);
        Assert.assertEquals(0.5625f, mLastRatio, 0.0001f);
        Assert.assertEquals(2000, mLastInitialFrame.getWidth());
        Assert.assertEquals(1125, mLastInitialFrame.getHeight());
        waitForThumbnailDecode(); // Rest of frames.
        Assert.assertTrue(mLastDecodedPath.contains(video1));
        Assert.assertEquals(true, mLastIsVideo);
        Assert.assertEquals("0:00", mLastVideoDuration);
        Assert.assertEquals(10, mLastFrameCount);
        Assert.assertEquals(0.5625f, mLastRatio, 0.0001f);
        Assert.assertEquals(2000, mLastInitialFrame.getWidth());
        Assert.assertEquals(1125, mLastInitialFrame.getHeight());

        host.unbind(mContext);
    }

    @Test
    @LargeTest
    @DisabledTest
    public void testCancelation() throws Throwable {
        DecoderServiceHost host = new DecoderServiceHost(this, mContext);
        host.bind(mContext);
        waitForDecoder();

        String green = "green100x100.jpg";
        String yellow = "yellow100x100.jpg";
        String red = "red100x100.jpg";
        String filePath = "chrome/test/data/android/photo_picker/";
        String greenPath = UrlUtils.getIsolatedTestFilePath(filePath + green);
        String yellowPath = UrlUtils.getIsolatedTestFilePath(filePath + yellow);
        String redPath = UrlUtils.getIsolatedTestFilePath(filePath + red);

        host.decodeImage(Uri.fromFile(new File(greenPath)), PickerBitmap.TileTypes.PICTURE, 10,
                /*fullWidth=*/false, this);
        host.decodeImage(Uri.fromFile(new File(yellowPath)), PickerBitmap.TileTypes.PICTURE, 10,
                /*fullWidth=*/false, this);

        // Now add and subsequently remove the request.
        host.decodeImage(Uri.fromFile(new File(redPath)), PickerBitmap.TileTypes.PICTURE, 10,
                /*fullWidth=*/false, this);
        host.cancelDecodeImage(redPath);

        // First decoding result should be the green image.
        waitForThumbnailDecode();
        Assert.assertEquals(greenPath, mLastDecodedPath);

        // Next is the yellow image, and asserts in DecoderServiceHost (designed to catch when
        // multiple simultaneous decoding requests are started) should not fire.
        waitForThumbnailDecode();
        Assert.assertEquals(yellowPath, mLastDecodedPath);

        host.unbind(mContext);
    }
}
