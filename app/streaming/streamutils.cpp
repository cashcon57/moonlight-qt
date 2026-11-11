#include "streamutils.h"

#include <Qt>
#include <QDir>
#include <QFile>

#ifdef Q_OS_DARWIN
#include <ApplicationServices/ApplicationServices.h>
#endif

#ifdef Q_OS_WINDOWS
#include <Windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

#ifdef Q_OS_UNIX
#include <unistd.h>
#include <fcntl.h>

#include <SDL_syswm.h>
#endif

#ifdef Q_OS_LINUX
#include <sys/auxv.h>

#if defined(Q_PROCESSOR_ARM)

#ifndef HWCAP2_AES
#define HWCAP2_AES (1 << 0)
#endif

#elif defined(Q_PROCESSOR_RISCV)

#if __has_include(<sys/hwprobe.h>)
#include <sys/hwprobe.h>
#else
#include <unistd.h>

#if __has_include(<asm/hwprobe.h>)
#include <asm/hwprobe.h>
#include <sys/syscall.h>
#else
struct riscv_hwprobe {
    int64_t key;
    uint64_t value;
};
#define RISCV_HWPROBE_KEY_IMA_EXT_0 4
#endif

#ifndef __NR_riscv_hwprobe
#define __NR_riscv_hwprobe 258
#endif

// RISC-V Scalar AES [E]ncryption and [D]ecryption
#ifndef RISCV_HWPROBE_EXT_ZKND
#define RISCV_HWPROBE_EXT_ZKND (1 << 11)
#define RISCV_HWPROBE_EXT_ZKNE (1 << 12)
#endif

// RISC-V Vector AES
#ifndef RISCV_HWPROBE_EXT_ZVKNED
#define RISCV_HWPROBE_EXT_ZVKNED (1 << 21)
#endif

static int __riscv_hwprobe(struct riscv_hwprobe *pairs, size_t pair_count,
                           size_t cpu_count, unsigned long *cpus,
                           unsigned int flags)
{
    return syscall(__NR_riscv_hwprobe, pairs, pair_count, cpu_count, cpus, flags);
}

#endif
#endif
#endif

#if defined(Q_OS_FREEBSD) || defined(Q_OS_OPENBSD)
#include <sys/auxv.h>
#endif

Uint32 StreamUtils::getPlatformWindowFlags()
{
#if defined(HAVE_LIBPLACEBO_VULKAN)
    // We'll fall back to GL if Vulkan fails
    return SDL_WINDOW_VULKAN;
#elif defined(Q_OS_DARWIN)
    // Vulkan needs to supersede Metal, otherwise the Vulkan library won't be loaded
    return SDL_WINDOW_METAL;
#else
    return 0;
#endif
}

SDL_Window* StreamUtils::createTestWindow()
{
    SDL_Window* testWindow;
    Uint32 baseFlags = 0;

    // Stop text input before creating the test window to avoid sdl2-compat
    // starting text input on the new window. This might trigger the IME to
    // be displayed.
    SDL_StopTextInput();

    // Test windows are always hidden
    baseFlags |= SDL_WINDOW_HIDDEN;

    // Creating a Vulkan surface with KMSDRM requires finding a display mode
    // that exactly matches the window size. This is not always possible,
    // particularly with drivers (Nvidia) that only expose modes matching
    // the current resolution (only differing by refresh rate). Fullscreen
    // desktop mode ensures the window size exactly matches the display mode
    // which prevents false Vulkan renderer failures during decoder probing.
    if (QString(SDL_GetCurrentVideoDriver()) == "KMSDRM") {
        baseFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    // Try to add the platform-specific flags first and fall back if that fails
    testWindow = SDL_CreateWindow("", 0, 0, 1280, 720,
                                  baseFlags | StreamUtils::getPlatformWindowFlags());
    if (!testWindow) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to create test window with platform flags: %s",
                    SDL_GetError());

        testWindow = SDL_CreateWindow("", 0, 0, 1280, 720, baseFlags);
        if (!testWindow) {
            return nullptr;
        }
    }

    return testWindow;
}

void StreamUtils::scaleSourceToDestinationSurface(SDL_Rect* src, SDL_Rect* dst)
{
    int dstH = SDL_ceilf((float)dst->w * src->h / src->w);
    int dstW = SDL_ceilf((float)dst->h * src->w / src->h);

    if (dstH > dst->h) {
        dst->x += (dst->w - dstW) / 2;
        dst->w = dstW;
    }
    else {
        dst->y += (dst->h - dstH) / 2;
        dst->h = dstH;
    }
}

void StreamUtils::screenSpaceToNormalizedDeviceCoords(SDL_FRect* rect, int viewportWidth, int viewportHeight)
{
    rect->x = (rect->x / (viewportWidth / 2.0f)) - 1.0f;
    rect->y = (rect->y / (viewportHeight / 2.0f)) - 1.0f;
    rect->w = rect->w / (viewportWidth / 2.0f);
    rect->h = rect->h / (viewportHeight / 2.0f);
}

void StreamUtils::screenSpaceToNormalizedDeviceCoords(SDL_Rect* src, SDL_FRect* dst, int viewportWidth, int viewportHeight)
{
    dst->x = ((float)src->x / (viewportWidth / 2.0f)) - 1.0f;
    dst->y = ((float)src->y / (viewportHeight / 2.0f)) - 1.0f;
    dst->w = (float)src->w / (viewportWidth / 2.0f);
    dst->h = (float)src->h / (viewportHeight / 2.0f);
}

int StreamUtils::getDisplayRefreshRate(SDL_Window* window)
{
    int displayIndex = SDL_GetWindowDisplayIndex(window);
    if (displayIndex < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to get current display: %s",
                     SDL_GetError());

        // Assume display 0 if it fails
        displayIndex = 0;
    }

    SDL_DisplayMode mode;
    if ((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN) {
        // Use the window display mode for full-screen exclusive mode
        if (SDL_GetWindowDisplayMode(window, &mode) != 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_GetWindowDisplayMode() failed: %s",
                         SDL_GetError());

            // Assume 60 Hz
            return 60;
        }
    }
    else {
        // Use the current display mode for windowed and borderless
        if (SDL_GetCurrentDisplayMode(displayIndex, &mode) != 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_GetCurrentDisplayMode() failed: %s",
                         SDL_GetError());

            // Assume 60 Hz
            return 60;
        }
    }

    // May be zero if undefined
    if (mode.refresh_rate == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Refresh rate unknown; assuming 60 Hz");
        mode.refresh_rate = 60;
    }

    return mode.refresh_rate;
}

#if defined(Q_OS_WINDOWS)

static uint16_t clampToUint16(float value)
{
    return (uint16_t)SDL_clamp(value + 0.5f, 0.0f, 65535.0f);
}

static bool getDxgiHdrCapabilities(int displayIndex, PSS_HDR_METADATA metadata, QString& source)
{
    int adapterIndex, outputIndex;
    if (!SDL_DXGIGetOutputInfo(displayIndex, &adapterIndex, &outputIndex)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_DXGIGetOutputInfo() failed: %s",
                     SDL_GetError());
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
    DXGI_OUTPUT_DESC1 desc;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) {
        hr = factory->EnumAdapters1(adapterIndex, &adapter);
    }
    if (SUCCEEDED(hr)) {
        hr = adapter->EnumOutputs(outputIndex, &output);
    }
    if (SUCCEEDED(hr)) {
        hr = output.As(&output6);
    }
    if (SUCCEEDED(hr)) {
        hr = output6->GetDesc1(&desc);
    }
    if (FAILED(hr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to get DXGI_OUTPUT_DESC1: %x",
                    hr);
        return false;
    }

    // Chromaticity coordinates are normalized to 50,000
    const FLOAT* primaries[] = { desc.RedPrimary, desc.GreenPrimary, desc.BluePrimary };
    for (int i = 0; i < 3; i++) {
        metadata->displayPrimaries[i].x = clampToUint16(primaries[i][0] * 50000);
        metadata->displayPrimaries[i].y = clampToUint16(primaries[i][1] * 50000);
    }
    metadata->whitePoint.x = clampToUint16(desc.WhitePoint[0] * 50000);
    metadata->whitePoint.y = clampToUint16(desc.WhitePoint[1] * 50000);

    metadata->maxDisplayLuminance = clampToUint16(desc.MaxLuminance);
    metadata->minDisplayLuminance = clampToUint16(desc.MinLuminance * 10000);
    metadata->maxFullFrameLuminance = clampToUint16(desc.MaxFullFrameLuminance);

    source = "DXGI";
    return true;
}

#elif defined(Q_OS_LINUX)

static bool isEdidBlockValid(const QByteArray& edid, int offset)
{
    if (edid.size() < offset + 128) {
        return false;
    }

    uint8_t sum = 0;
    for (int i = 0; i < 128; i++) {
        sum += (uint8_t)edid[offset + i];
    }
    return sum == 0;
}

static QByteArray getEdidMonitorName(const QByteArray& edid)
{
    if (!isEdidBlockValid(edid, 0)) {
        return QByteArray();
    }

    // Look for a Display Product Name descriptor (tag 0xFC) in the base block
    for (int offset = 54; offset <= 108; offset += 18) {
        const uint8_t* desc = (const uint8_t*)edid.constData() + offset;
        if (desc[0] == 0 && desc[1] == 0 && desc[2] == 0 && desc[3] == 0xFC) {
            return QByteArray((const char*)&desc[5], 13).split('\n').first().trimmed();
        }
    }

    return QByteArray();
}

static bool parseEdidHdrCapabilities(const QByteArray& edid, PSS_HDR_METADATA metadata)
{
    static const char header[] = { 0x00, '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', 0x00 };
    if (!isEdidBlockValid(edid, 0) || !edid.startsWith(QByteArray::fromRawData(header, sizeof(header)))) {
        return false;
    }

    // The base block has 10-bit chromaticity coordinates (normalized to 1024) at offset 25:
    // the low bits of red and green, the low bits of blue and white, then the high bits of
    // Rx, Ry, Gx, Gy, Bx, By, Wx, and Wy.
    const uint8_t* e = (const uint8_t*)edid.constData();
    uint16_t xy[8];
    for (int i = 0; i < 8; i++) {
        uint16_t value = (e[27 + i] << 2) | ((e[25 + i / 4] >> (6 - 2 * (i % 4))) & 0x3);
        xy[i] = (uint16_t)((value * 50000 + 512) / 1024);
    }
    for (int i = 0; i < 3; i++) {
        metadata->displayPrimaries[i].x = xy[i * 2];
        metadata->displayPrimaries[i].y = xy[i * 2 + 1];
    }
    metadata->whitePoint.x = xy[6];
    metadata->whitePoint.y = xy[7];

    // Look for the HDR Static Metadata Data Block (CTA-861.3) in the CTA extension blocks
    for (int ext = 1; ext <= e[126] && isEdidBlockValid(edid, ext * 128); ext++) {
        const uint8_t* cta = e + ext * 128;
        if (cta[0] != 0x02 || cta[2] < 4 || cta[2] > 127) {
            continue;
        }

        for (int i = 4; i < cta[2]; i += (cta[i] & 0x1F) + 1) {
            int length = cta[i] & 0x1F;

            // Extended tag (7) with extended tag code 6, Static Metadata Type 1 support,
            // and at least the max luminance field present
            if ((cta[i] >> 5) != 7 || length < 4 || i + length >= cta[2] ||
                    cta[i + 1] != 6 || !(cta[i + 3] & 0x1)) {
                continue;
            }

            // Luminance is 50 * 2^(CV / 32) nits, and code values of 0 mean the
            // display didn't provide that value
            auto luminance = [](uint8_t codeValue) {
                return codeValue != 0 ? 50.0 * SDL_pow(2.0, codeValue / 32.0) : 0.0;
            };
            double maxLuminance = luminance(cta[i + 4]);
            metadata->maxDisplayLuminance = (uint16_t)SDL_lround(maxLuminance);
            if (length >= 5) {
                metadata->maxFullFrameLuminance = (uint16_t)SDL_lround(luminance(cta[i + 5]));
            }
            if (length >= 6) {
                // Minimum luminance is max * (CV / 255)^2 / 100 nits, in 1/10000th of a nit
                double minLuminance = maxLuminance * SDL_pow(cta[i + 6] / 255.0, 2) / 100;
                metadata->minDisplayLuminance = (uint16_t)SDL_min(SDL_lround(minLuminance * 10000), 65535L);
            }
            return true;
        }
    }

    return true;
}

static bool getEdidHdrCapabilities(int displayIndex, PSS_HDR_METADATA metadata, QString& source)
{
    // Find the connected DRM connectors. The EDID is world-readable in sysfs, and this
    // works whether we're running under X11, Wayland, or directly on KMSDRM.
    QDir drmDir("/sys/class/drm");
    QList<QPair<QString, QByteArray>> connectors;
    for (const QString& name : drmDir.entryList(QStringList("card*-*"), QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile status(drmDir.filePath(name + "/status"));
        QFile edid(drmDir.filePath(name + "/edid"));
        if (status.open(QIODevice::ReadOnly) && status.readAll().trimmed() == "connected" &&
                edid.open(QIODevice::ReadOnly)) {
            QByteArray edidData = edid.readAll();
            if (!edidData.isEmpty()) {
                connectors.append(qMakePair(name, edidData));
            }
        }
    }

    // If there are multiple displays, we must be able to identify the one we're using
    // by its connector name (e.g. "HDMI-A-1") or EDID product name.
    if (connectors.size() > 1) {
        QByteArray displayName = SDL_GetDisplayName(displayIndex);
        QList<QPair<QString, QByteArray>> matches;
        for (const auto& connector : std::as_const(connectors)) {
            QByteArray connectorName = connector.first.section('-', 1).toUtf8();
            QByteArray monitorName = getEdidMonitorName(connector.second);
            if (!displayName.isEmpty() &&
                    (displayName.contains(connectorName) || (!monitorName.isEmpty() && displayName.contains(monitorName)))) {
                matches.append(connector);
            }
        }

        if (matches.size() != 1) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to find the DRM connector for display '%s' (%d connected, %d matches)",
                        displayName.constData(), (int)connectors.size(), (int)matches.size());
            return false;
        }
        connectors = matches;
    }

    if (connectors.size() != 1 || !parseEdidHdrCapabilities(connectors.first().second, metadata)) {
        return false;
    }

    source = "EDID of " + connectors.first().first;
    return true;
}

#endif

bool StreamUtils::getDisplayHdrCapabilities(int displayIndex, PSS_HDR_METADATA metadata)
{
    bool ret = false;
    QString source;

    SDL_zerop(metadata);

#if defined(Q_OS_WINDOWS)
    ret = getDxgiHdrCapabilities(displayIndex, metadata, source);
#elif defined(Q_OS_LINUX)
    ret = getEdidHdrCapabilities(displayIndex, metadata, source);
#else
    Q_UNUSED(displayIndex);
#endif

    if (!ret) {
        // Unknown values are sent as zeros
        SDL_zerop(metadata);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Display HDR capabilities are unavailable");
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Display HDR capabilities from %s: R(%.4f, %.4f) G(%.4f, %.4f) B(%.4f, %.4f) W(%.4f, %.4f) "
                "max %u nits, min %.4f nits, max full-frame %u nits",
                qPrintable(source),
                metadata->displayPrimaries[0].x / 50000.0, metadata->displayPrimaries[0].y / 50000.0,
                metadata->displayPrimaries[1].x / 50000.0, metadata->displayPrimaries[1].y / 50000.0,
                metadata->displayPrimaries[2].x / 50000.0, metadata->displayPrimaries[2].y / 50000.0,
                metadata->whitePoint.x / 50000.0, metadata->whitePoint.y / 50000.0,
                metadata->maxDisplayLuminance,
                metadata->minDisplayLuminance / 10000.0,
                metadata->maxFullFrameLuminance);
    return true;
}

bool StreamUtils::hasFastAes()
{
#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#if (__has_builtin(__builtin_cpu_supports) || (defined(__GNUC__) && __GNUC__ >= 6)) && defined(Q_PROCESSOR_X86)
    return __builtin_cpu_supports("aes");
#elif defined(__BUILTIN_CPU_SUPPORTS__) && defined(Q_PROCESSOR_POWER)
    return __builtin_cpu_supports("vcrypto");
#elif defined(Q_OS_WINDOWS) && defined(Q_PROCESSOR_ARM)
    return IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE);
#elif defined(_MSC_VER) && defined(Q_PROCESSOR_X86)
    int regs[4];
    __cpuid(regs, 1);
    return regs[2] & (1 << 25); // AES-NI
#elif defined(Q_OS_DARWIN)
    // Everything that runs Catalina and later has AES-NI or ARMv8 crypto instructions
    return true;
#elif (defined(Q_OS_FREEBSD) || defined(Q_OS_OPENBSD)) && defined(Q_PROCESSOR_ARM) && QT_POINTER_SIZE == 4
    unsigned long hwcap2;
    if (elf_aux_info(AT_HWCAP2, &hwcap2, sizeof(hwcap2)) != 0)
        hwcap2 = 0;
    return (hwcap2 & HWCAP2_AES);
#elif (defined(Q_OS_FREEBSD) || defined(Q_OS_OPENBSD)) && defined(Q_PROCESSOR_ARM) && QT_POINTER_SIZE == 8
    unsigned long hwcap;
    if (elf_aux_info(AT_HWCAP, &hwcap, sizeof(hwcap)) != 0)
        hwcap = 0;
    return (hwcap & HWCAP_AES);
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_ARM) && QT_POINTER_SIZE == 4
    return getauxval(AT_HWCAP2) & HWCAP2_AES;
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_ARM) && QT_POINTER_SIZE == 8
    return getauxval(AT_HWCAP) & HWCAP_AES;
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_RISCV)
    riscv_hwprobe pairs[1] = {
        { RISCV_HWPROBE_KEY_IMA_EXT_0, 0 },
    };

    // If this syscall is not implemented, we'll get -ENOSYS
    // and the value field will remain zero.
    __riscv_hwprobe(pairs, SDL_arraysize(pairs), 0, nullptr, 0);

    return (pairs[0].value & (RISCV_HWPROBE_EXT_ZKNE | RISCV_HWPROBE_EXT_ZKND)) ==
               (RISCV_HWPROBE_EXT_ZKNE | RISCV_HWPROBE_EXT_ZKND) ||
           (pairs[0].value & RISCV_HWPROBE_EXT_ZVKNED);
#elif QT_POINTER_SIZE == 4
    #warning Unknown 32-bit platform. Assuming AES is slow on this CPU.
    return false;
#else
    #warning Unknown 64-bit platform. Assuming AES is fast on this CPU.
    return true;
#endif
}

bool StreamUtils::getNativeDesktopMode(int displayIndex, SDL_DisplayMode* mode, SDL_Rect* safeArea)
{
#ifdef Q_OS_DARWIN
#define MAX_DISPLAYS 16
    CGDirectDisplayID displayIds[MAX_DISPLAYS];
    uint32_t displayCount = 0;
    CGGetActiveDisplayList(MAX_DISPLAYS, displayIds, &displayCount);
    if (displayIndex >= (int)displayCount) {
        return false;
    }

    SDL_zerop(mode);

    // Retina displays have non-native resolutions both below and above (!) their
    // native resolution, so it's impossible for us to figure out what's actually
    // native on macOS using the SDL API alone. We'll talk to CoreGraphics to
    // find the correct resolution and match it in our SDL list.
    CFArrayRef modeList = CGDisplayCopyAllDisplayModes(displayIds[displayIndex], nullptr);
    CFIndex count = CFArrayGetCount(modeList);
    for (CFIndex i = 0; i < count; i++) {
        auto cgMode = (CGDisplayModeRef)(CFArrayGetValueAtIndex(modeList, i));
        if ((CGDisplayModeGetIOFlags(cgMode) & kDisplayModeNativeFlag) != 0) {
            mode->w = static_cast<int>(CGDisplayModeGetWidth(cgMode));
            mode->h = static_cast<int>(CGDisplayModeGetHeight(cgMode));
            break;
        }
    }

    safeArea->x = 0;
    safeArea->y = 0;
    safeArea->w = mode->w;
    safeArea->h = mode->h;

#if TARGET_CPU_ARM64
    // Now that we found the native full-screen mode, let's look for one that matches along
    // the width but not the height and we'll assume that's the safe area full-screen mode.
    //
    // There doesn't appear to be a CG API or flag that will tell us that a given mode
    // is a "safe area" mode, so we have to use our own (brittle) heuristics. :(
    //
    // To avoid potential false positives, let's avoid checking for external displays, since
    // we might have scenarios like a 1920x1200 display with an alternate 1920x1080 mode
    // which would falsely trigger our notch detection here.
    if (CGDisplayIsBuiltin(displayIds[displayIndex])) {
        for (CFIndex i = 0; i < count; i++) {
            auto cgMode = (CGDisplayModeRef)(CFArrayGetValueAtIndex(modeList, i));
            auto cgModeWidth = static_cast<int>(CGDisplayModeGetWidth(cgMode));
            auto cgModeHeight = static_cast<int>(CGDisplayModeGetHeight(cgMode));

            // If the modes differ by more than 100, we'll assume it's not a notch mode
            if (mode->w == cgModeWidth && mode->h != cgModeHeight && mode->h <= cgModeHeight + 100) {
                safeArea->w = cgModeWidth;
                safeArea->h = cgModeHeight;
            }
        }
    }
#endif

    CFRelease(modeList);

    // Special case for probing for notched displays prior to video subsystem initialization
    // in Session::initialize() for Darwin only!
    if (SDL_WasInit(SDL_INIT_VIDEO)) {
        // Now find the SDL mode that matches the CG native mode
        for (int i = 0; i < SDL_GetNumDisplayModes(displayIndex); i++) {
            SDL_DisplayMode thisMode;
            if (SDL_GetDisplayMode(displayIndex, i, &thisMode) == 0) {
                if (thisMode.w == mode->w && thisMode.h == mode->h &&
                    thisMode.refresh_rate >= mode->refresh_rate) {
                    *mode = thisMode;
                    break;
                }
            }
        }
    }
#else
    SDL_assert(SDL_WasInit(SDL_INIT_VIDEO));

    if (displayIndex >= SDL_GetNumVideoDisplays()) {
        return false;
    }

    // We need to get the true display resolution without DPI scaling (since we use High DPI).
    // Windows returns the real display resolution here, even if DPI scaling is enabled.
    // macOS and Wayland report a resolution that includes the DPI scaling factor. Picking
    // the first mode on Wayland will get the native resolution without the scaling factor
    // (and macOS is handled in the #ifdef above).
    if (!strcmp(SDL_GetCurrentVideoDriver(), "wayland")) {
        if (SDL_GetDisplayMode(displayIndex, 0, mode) != 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_GetDisplayMode() failed: %s",
                         SDL_GetError());
            return false;
        }
    }
    else {
        if (SDL_GetDesktopDisplayMode(displayIndex, mode) != 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_GetDesktopDisplayMode() failed: %s",
                         SDL_GetError());
            return false;
        }
    }

    safeArea->x = 0;
    safeArea->y = 0;
    safeArea->w = mode->w;
    safeArea->h = mode->h;
#endif

    return true;
}

int StreamUtils::getDrmFdForWindow(SDL_Window* window, bool* mustClose)
{
    *mustClose = false;

#if defined(SDL_VIDEO_DRIVER_KMSDRM) && SDL_VERSION_ATLEAST(2, 0, 15)
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_GetWindowWMInfo() failed: %s",
                     SDL_GetError());
        return -1;
    }

    if (info.subsystem == SDL_SYSWM_KMSDRM) {
        // If SDL has an FD, share that
        if (info.info.kmsdrm.drm_fd >= 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Sharing DRM FD with SDL");
            return info.info.kmsdrm.drm_fd;
        }
        else {
            char path[128];
            snprintf(path, sizeof(path), "/dev/dri/card%u", info.info.kmsdrm.dev_index);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Opening DRM FD from SDL by path: %s",
                        path);
            int fd = open(path, O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                *mustClose = true;
            }
            return fd;
        }
    }
#else
    Q_UNUSED(window);
#endif

    return -1;
}

int StreamUtils::getDrmFd(bool preferRenderNode)
{
#ifdef Q_OS_UNIX
    const char* userDevice = SDL_getenv("DRM_DEV");
    if (userDevice != nullptr) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Opening user-specified DRM device: %s",
                    userDevice);

        return open(userDevice, O_RDWR | O_CLOEXEC);
    }
    else {
        QDir driDir("/dev/dri");
        int fd;

        // We have to explicitly ask for devices to be returned
        driDir.setFilter(QDir::Files | QDir::System);

        if (preferRenderNode) {
            // Try a render node first since we aren't using DRM for output in this codepath
            for (QFileInfo& node : driDir.entryInfoList(QStringList("renderD*"))) {
                QByteArray absolutePath = node.absoluteFilePath().toUtf8();
                fd = open(absolutePath.constData(), O_RDWR | O_CLOEXEC);
                if (fd >= 0) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Opened DRM render node: %s",
                                absolutePath.constData());
                    return fd;
                }
            }
        }

        // If that fails, try to use a primary node and hope for the best
        for (QFileInfo& node : driDir.entryInfoList(QStringList("card*"))) {
            QByteArray absolutePath = node.absoluteFilePath().toUtf8();
            fd = open(absolutePath.constData(), O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Opened DRM primary node: %s",
                            absolutePath.constData());
                return fd;
            }
        }
    }
#else
    Q_UNUSED(preferRenderNode);
#endif

    return -1;
}

extern QAtomicInt g_AsyncLoggingEnabled;

void StreamUtils::enterAsyncLoggingMode()
{
    g_AsyncLoggingEnabled.ref();
}

void StreamUtils::exitAsyncLoggingMode()
{
    g_AsyncLoggingEnabled.deref();
}
