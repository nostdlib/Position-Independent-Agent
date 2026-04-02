/**
 * @file screen.cc
 * @brief Android screen capture via SurfaceControl hidden API (JNI)
 *
 * @details Captures the screen by calling Android's hidden SurfaceControl
 * screenshot API via JNI reflection. The PIC agent thread is attached to
 * ART via AttachCurrentThreadAsDaemon (no classloader context), which
 * bypasses hidden API enforcement on Android 9+.
 *
 * Capture flow:
 *   1. JniBridgeAttach() → attach PIC thread to ART, get JNIEnv
 *   2. Reflect on android.view.SurfaceControl → find screenshot() method
 *   3. SurfaceControl.screenshot(Rect, width, height, rotation) → Bitmap
 *   4. Bitmap.getPixels() → int[] ARGB_8888 → convert to RGB
 *
 * This uses the same native path as screencap (SurfaceFlinger composites
 * all layers via GPU) but without spawning a child process.
 *
 * Permission: SurfaceFlinger checks calling UID at the Binder level.
 * Works when running as shell (UID 2000), system, graphics, or root.
 * Falls back to screencap/DRM/fbdev if SurfaceControl is unavailable.
 *
 * All state is stack-local — no static/global variables (PIC constraint).
 */

#include "platform/screen/screen.h"
#include "core/memory/memory.h"
#include "platform/kernel/android/jni.h"
#include "platform/kernel/android/syscall.h"
#include "platform/kernel/android/system.h"

// =============================================================================
// JNI helpers
// =============================================================================

static jclass SafeFindClass(JNIEnv *env, const CHAR *name)
{
	jclass cls = (*env)->FindClass((PVOID)env, name);
	if ((*env)->ExceptionOccurred((PVOID)env))
	{
		(*env)->ExceptionClear((PVOID)env);
		return nullptr;
	}
	return cls;
}

static jobject SafeCallStaticObjectMethod(JNIEnv *env, jclass cls, jmethodID mid)
{
	jobject result = (*env)->CallStaticObjectMethod((PVOID)env, cls, mid);
	if ((*env)->ExceptionOccurred((PVOID)env))
	{
		(*env)->ExceptionClear((PVOID)env);
		return nullptr;
	}
	return result;
}

static jint SafeCallIntMethod(JNIEnv *env, jobject obj, jmethodID mid)
{
	jint result = (*env)->CallIntMethod((PVOID)env, obj, mid);
	if ((*env)->ExceptionOccurred((PVOID)env))
	{
		(*env)->ExceptionClear((PVOID)env);
		return -1;
	}
	return result;
}

/// @brief Call void instance method via raw JNI table (index 61)
static VOID CallVoidMethod(JNIEnv *env, jobject obj, jmethodID mid)
{
	typedef void (*Fn)(PVOID, jobject, jmethodID, ...);
	((Fn)((PVOID *)*env)[61])((PVOID)env, obj, mid);
}

/// @brief Call static object method with jvalue args
static jobject SafeCallStaticObjectMethodA(JNIEnv *env, jclass cls, jmethodID mid, const jvalue *args)
{
	jobject result = (*env)->CallStaticObjectMethodA((PVOID)env, cls, mid, args);
	if ((*env)->ExceptionOccurred((PVOID)env))
	{
		(*env)->ExceptionClear((PVOID)env);
		return nullptr;
	}
	return result;
}

/// @brief GetIntArrayElements via raw JNI table (index 187)
static jint *GetIntArrayElements(JNIEnv *env, jintArray arr, jboolean *isCopy)
{
	typedef jint *(*Fn)(PVOID, jintArray, jboolean *);
	return ((Fn)((PVOID *)*env)[187])((PVOID)env, arr, isCopy);
}

/// @brief ReleaseIntArrayElements via raw JNI table (index 193)
static VOID ReleaseIntArrayElements(JNIEnv *env, jintArray arr, jint *elems, jint mode)
{
	typedef void (*Fn)(PVOID, jintArray, jint *, jint);
	((Fn)((PVOID *)*env)[193])((PVOID)env, arr, elems, mode);
}

/// @brief NewIntArray via raw JNI table (index 176)
static jintArray NewIntArray(JNIEnv *env, jsize len)
{
	typedef jintArray (*Fn)(PVOID, jsize);
	return ((Fn)((PVOID *)*env)[176])((PVOID)env, len);
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
	jmethodID currentApp = (*env)->GetStaticMethodID(
		(PVOID)env, atClass, (const CHAR *)currentAppName, (const CHAR *)currentAppSig);
	if (currentApp == nullptr)
		return false;

	jobject context = SafeCallStaticObjectMethod(env, atClass, currentApp);
	if (context == nullptr)
		return false;

	jclass ctxClass = (*env)->GetObjectClass((PVOID)env, context);
	auto getSvcName = "getSystemService";
	auto getSvcSig = "(Ljava/lang/String;)Ljava/lang/Object;";
	jmethodID getSvc = (*env)->GetMethodID((PVOID)env, ctxClass,
		(const CHAR *)getSvcName, (const CHAR *)getSvcSig);
	if (getSvc == nullptr)
		return false;

	auto windowStr = "window";
	jstring windowSvc = (*env)->NewStringUTF((PVOID)env, (const CHAR *)windowStr);
	jobject wm = (*env)->CallObjectMethod((PVOID)env, context, getSvc, windowSvc);
	(*env)->DeleteLocalRef((PVOID)env, windowSvc);
	if (wm == nullptr)
		return false;

	auto wmClassName = "android/view/WindowManager";
	jclass wmClass = SafeFindClass(env, (const CHAR *)wmClassName);
	if (wmClass == nullptr)
		return false;

	auto getDisplayName = "getDefaultDisplay";
	auto getDisplaySig = "()Landroid/view/Display;";
	jmethodID getDisplay = (*env)->GetMethodID(
		(PVOID)env, wmClass, (const CHAR *)getDisplayName, (const CHAR *)getDisplaySig);
	if (getDisplay == nullptr)
		return false;

	jobject display = (*env)->CallObjectMethod((PVOID)env, wm, getDisplay);
	if (display == nullptr)
		return false;

	jclass displayClass = (*env)->GetObjectClass((PVOID)env, display);
	auto intRetSig = "()I";

	auto getWidthName = "getWidth";
	auto getHeightName = "getHeight";
	jmethodID getWidth = (*env)->GetMethodID(
		(PVOID)env, displayClass, (const CHAR *)getWidthName, (const CHAR *)intRetSig);
	jmethodID getHeight = (*env)->GetMethodID(
		(PVOID)env, displayClass, (const CHAR *)getHeightName, (const CHAR *)intRetSig);

	if (getWidth == nullptr || getHeight == nullptr)
		return false;

	width = (UINT32)SafeCallIntMethod(env, display, getWidth);
	height = (UINT32)SafeCallIntMethod(env, display, getHeight);

	return (width > 0 && height > 0);
}

// =============================================================================
// SurfaceControl.screenshot() via JNI reflection
// =============================================================================

/// @brief Attempt to capture screen via SurfaceControl hidden API
/// @param env JNI environment
/// @param width Display width
/// @param height Display height
/// @param rgbBuf Output RGB buffer (width * height elements)
/// @return true on success
static BOOL SurfaceControlCapture(JNIEnv *env, UINT32 width, UINT32 height, PRGB rgbBuf)
{
	// Find SurfaceControl class
	auto scClassName = "android/view/SurfaceControl";
	jclass scClass = SafeFindClass(env, (const CHAR *)scClassName);
	if (scClass == nullptr)
		return false;

	// Try Android 9+ signature:
	// static Bitmap screenshot(Rect sourceCrop, int width, int height, int rotation)
	auto screenshotName = "screenshot";
	auto screenshotSig = "(Landroid/graphics/Rect;III)Landroid/graphics/Bitmap;";
	jmethodID screenshotMethod = (*env)->GetStaticMethodID(
		(PVOID)env, scClass, (const CHAR *)screenshotName, (const CHAR *)screenshotSig);

	if ((*env)->ExceptionOccurred((PVOID)env))
		(*env)->ExceptionClear((PVOID)env);

	jobject bitmap = nullptr;

	if (screenshotMethod != nullptr)
	{
		// Call: SurfaceControl.screenshot(null, width, height, 0)
		jvalue args[4];
		args[0].l = nullptr;          // null Rect = full screen
		args[1].i = (jint)width;
		args[2].i = (jint)height;
		args[3].i = 0;                // ROTATION_0
		bitmap = SafeCallStaticObjectMethodA(env, scClass, screenshotMethod, args);
	}

	// Try alternative signature (older Android):
	// static Bitmap screenshot(int width, int height)
	if (bitmap == nullptr)
	{
		auto altSig = "(II)Landroid/graphics/Bitmap;";
		jmethodID altMethod = (*env)->GetStaticMethodID(
			(PVOID)env, scClass, (const CHAR *)screenshotName, (const CHAR *)altSig);
		if ((*env)->ExceptionOccurred((PVOID)env))
			(*env)->ExceptionClear((PVOID)env);

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

	// Bitmap.getWidth(), Bitmap.getHeight()
	jclass bmpClass = (*env)->GetObjectClass((PVOID)env, bitmap);
	auto intRetSig = "()I";

	auto bmpGetWidthName = "getWidth";
	auto bmpGetHeightName = "getHeight";
	jmethodID bmpGetWidth = (*env)->GetMethodID(
		(PVOID)env, bmpClass, (const CHAR *)bmpGetWidthName, (const CHAR *)intRetSig);
	jmethodID bmpGetHeight = (*env)->GetMethodID(
		(PVOID)env, bmpClass, (const CHAR *)bmpGetHeightName, (const CHAR *)intRetSig);

	if (bmpGetWidth == nullptr || bmpGetHeight == nullptr)
		return false;

	jint bmpW = SafeCallIntMethod(env, bitmap, bmpGetWidth);
	jint bmpH = SafeCallIntMethod(env, bitmap, bmpGetHeight);

	if (bmpW <= 0 || bmpH <= 0)
		return false;

	UINT32 bmpWidth = (UINT32)bmpW;
	UINT32 bmpHeight = (UINT32)bmpH;
	if (bmpWidth > width) bmpWidth = width;
	if (bmpHeight > height) bmpHeight = height;

	// Bitmap.getPixels(int[] pixels, int offset, int stride, int x, int y, int width, int height)
	USIZE pixelCount = (USIZE)bmpWidth * (USIZE)bmpHeight;
	jintArray pixelArray = NewIntArray(env, (jsize)pixelCount);
	if (pixelArray == nullptr)
		return false;

	auto getPixelsName = "getPixels";
	auto getPixelsSig = "([IIIIIII)V";
	jmethodID getPixels = (*env)->GetMethodID(
		(PVOID)env, bmpClass, (const CHAR *)getPixelsName, (const CHAR *)getPixelsSig);
	if (getPixels == nullptr)
		return false;

	// Call getPixels via CallVoidMethodA (index 63)
	typedef void (*FnCallVoidMethodA)(PVOID, jobject, jmethodID, const jvalue *);
	FnCallVoidMethodA callVoidA = (FnCallVoidMethodA)((PVOID *)*env)[63];

	jvalue gpArgs[7];
	gpArgs[0].l = pixelArray;
	gpArgs[1].i = 0;                    // offset
	gpArgs[2].i = (jint)bmpWidth;       // stride
	gpArgs[3].i = 0;                    // x
	gpArgs[4].i = 0;                    // y
	gpArgs[5].i = (jint)bmpWidth;       // width
	gpArgs[6].i = (jint)bmpHeight;      // height

	callVoidA((PVOID)env, bitmap, getPixels, gpArgs);

	if ((*env)->ExceptionOccurred((PVOID)env))
	{
		(*env)->ExceptionClear((PVOID)env);
		return false;
	}

	// Read pixel data: ARGB_8888 int array → RGB
	jint *pixels = GetIntArrayElements(env, pixelArray, nullptr);
	if (pixels == nullptr)
		return false;

	for (UINT32 y = 0; y < bmpHeight; y++)
	{
		for (UINT32 x = 0; x < bmpWidth; x++)
		{
			UINT32 argb = (UINT32)pixels[y * bmpWidth + x];
			rgbBuf[y * width + x].Red   = (UINT8)((argb >> 16) & 0xFF);
			rgbBuf[y * width + x].Green = (UINT8)((argb >> 8) & 0xFF);
			rgbBuf[y * width + x].Blue  = (UINT8)(argb & 0xFF);
		}
	}

	ReleaseIntArrayElements(env, pixelArray, pixels, 0);

	// Recycle bitmap
	auto recycleName = "recycle";
	auto recycleSig = "()V";
	jmethodID recycle = (*env)->GetMethodID(
		(PVOID)env, bmpClass, (const CHAR *)recycleName, (const CHAR *)recycleSig);
	if (recycle != nullptr)
		CallVoidMethod(env, bitmap, recycle);

	return true;
}

// =============================================================================
// Public API — called from posix/screen.cc
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

	// Verify SurfaceControl class is accessible before reporting device
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

	PRGB rgbBuf = buffer.Data();

	if (!SurfaceControlCapture(env, device.Width, device.Height, rgbBuf))
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	return Result<VOID, Error>::Ok();
}
