#include "PrimeBridge.hpp"
#include "version.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/sysctl.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/time.h>
#include <sys/kauth.h>
#include <sys/proc.h>

#define PBLOG(fmt, ...) do { IOLog("PrimeBridge: " fmt "\n", ##__VA_ARGS__); } while (0)

OSDefineMetaClassAndAbstractStructors(PrimeFrameSource, OSObject)
OSDefineMetaClassAndStructors(PrimeBridge, IOService)

// iGPU register layouts. All fields are dword offsets into BAR5 (the MMIO
// aperture), exactly like Linux amdgpu's SOC15_REG_OFFSET(ip, 0, reg) =
// reg_offset[ip][0][reg_BASE_IDX] + reg; the byte offset is (base + reg) * 4.
struct IgpuRegLayout {
    const char *name;
    uint32_t    gcBase;      // GC segment 0 base, in dwords
    uint32_t    dcnBase;     // DCN/DMU segment 2 base, in dwords
    uint32_t    fbOffset;    // GCMC_VM_FB_OFFSET
    uint32_t    fbLocBase;   // GCMC_VM_FB_LOCATION_BASE
    uint32_t    fbLocTop;    // GCMC_VM_FB_LOCATION_TOP
    uint32_t    otg0Ctl;     // OTG0_OTG_CONTROL
    uint32_t    otg0HTotal;  // OTG0_OTG_H_TOTAL
    uint32_t    otg0VTotal;  // OTG0_OTG_V_TOTAL
    uint32_t    otg0Frames;  // OTG0_OTG_STATUS_FRAME_COUNT
    uint32_t    otgStride;   // dwords between OTG instances
};

// Radeon 680M (Rembrandt: GC 10.3.3, DCN 3.1.2), the layout this project shipped
// with. yellow_carp_offset.h: GC seg0 0x1260, DCN seg2 0x34C0.
// gc_10_3_0_offset.h: 0x16e7 / 0x16fc / 0x16fd.
// dcn_3_1_2_offset.h: OTG0 0x1b41 / 0x1b2a / 0x1b2f / 0x1b4c, stride 0x80.
static constexpr IgpuRegLayout kLayoutRembrandt = {
    "680M (GC 10.3.3 / DCN 3.1.2)",
    0x1260, 0x34C0, 0x16e7, 0x16fc, 0x16fd,
    0x1b41, 0x1b2a, 0x1b2f, 0x1b4c, 0x80,
};

// Phoenix (740M/760M/780M, GC 11.0.0-11.0.2, DCN 3.1.x). gc_11_0_0_offset.h:
// GCMC_VM_FB_OFFSET 0x15a7, GCMC_VM_FB_LOCATION_BASE/TOP 0x1678 / 0x1679.
static constexpr IgpuRegLayout kLayoutPhoenix = {
    "Phoenix (GC 11.0.x / DCN 3.1.x)",
    0x1260, 0x34C0, 0x15a7, 0x1678, 0x1679,
    0x1b41, 0x1b2a, 0x1b2f, 0x1b4c, 0x80,
};

// Hawk Point / Hawk Point 2 (760M/780M/740M, GC 11.0.3-11.0.4). gc_11_0_3_offset.h
// moved GCMC_VM_FB_LOCATION_BASE/TOP to 0x1688 / 0x1689 (GCMC_VM_FB_OFFSET is still
// 0x15a7). DCN 3.1.4 OTG offsets and the SOC15 segment bases are the same.
static constexpr IgpuRegLayout kLayoutHawkPoint = {
    "HawkPoint (GC 11.0.3/11.0.4 / DCN 3.1.4)",
    0x1260, 0x34C0, 0x15a7, 0x1688, 0x1689,
    0x1b41, 0x1b2a, 0x1b2f, 0x1b4c, 0x80,
};

static constexpr uint32_t kBar5MapLen   = 0x80000;  // mapped window of BAR5

// GC 11 APUs keep the VRAM window in the MMHUB block, not the GC block: Linux's
// gmc_v11_0 -> mmhub_v3_0_get_fb_location reads MMMC_VM_FB_LOCATION_BASE/TOP there.
// Offsets are from mmhub_3_0_0_offset.h; the segment base comes from IP discovery.
static constexpr uint32_t kMmFbLocBase  = 0x08ec;   // MMMC_VM_FB_LOCATION_BASE
static constexpr uint32_t kMmFbLocTop   = 0x08ed;   // MMMC_VM_FB_LOCATION_TOP

// Panel backlight PWM, DCN segment 2 (dcn_3_1_4_offset.h / _sh_mask.h):
// BL_ACTIVE_INT_FRAC_CNT [15:0] is the duty, BL_PWM_EN is bit 31.
static constexpr uint32_t kBlPwmCntl    = 0x2f19;   // PWRSEQ0_BL_PWM_CNTL

extern "C" int rdmsr_carefully(uint32_t msr, uint32_t *lo, uint32_t *hi);   // com.apple.kpi.unsupported

// Refresh rate in 1/100 Hz from two reads of the 24-bit OTG frame counter taken ns apart.
static constexpr uint32_t hz100(uint32_t f0, uint32_t f1, uint64_t ns) {
    return ns ? (uint32_t)((uint64_t)((f1 - f0) & 0xFFFFFFu) * 100000000000ULL / ns) : 0;
}
static_assert(hz100(0xFFFFF0u, 0x0006B0u, 12000000000ULL) == 14400, "1728 frames in 12 s across the wrap = 144.00 Hz");

// ---------------------------------------------------------------------------
// On-disk log sink. Every line is BOTH sent to IOLog (kernel ring buffer) AND
// appended to an in-RAM ring buffer that the tick loop flushes to a text file.
// The file target is /Volumes/1401/PrimeBridge.log (the USB key), which is not
// mounted when the kext starts; logFlush() opens it lazily with append and,
// until it succeeds, keeps lines buffered in RAM. The ring buffer drops the
// oldest lines if the flush cannot keep up, so a stuck mount can never panic
// or unboundedly grow the kernel heap.
// ---------------------------------------------------------------------------

void PrimeBridge::logf(const char *fmt, ...) {
    if (!logEnabled_) return;

    // Always mirror to the kernel log first (even if the ring buffer is not set up).
    {
        va_list va;
        va_start(va, fmt);
        char line[512];
        vsnprintf(line, sizeof(line), fmt, va);
        va_end(va);
        IOLog("PrimeBridge: %s\n", line);
    }

    if (!logBuf_ || logBufCap_ == 0) return;

    struct timeval tv;
    microuptime(&tv);

    char line[768];
    va_list va;
    va_start(va, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, va);
    va_end(va);
    if (n < 0) return;
    if ((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;

    // "[<sec>.<usec>] <line>\n"
    char stamped[900];
    int sn = snprintf(stamped, sizeof(stamped), "[%lld.%06lld] %s\n",
                      (long long)tv.tv_sec, (long long)tv.tv_usec, line);
    if (sn < 0) return;
    const size_t want = (size_t)sn < sizeof(stamped) ? (size_t)sn : sizeof(stamped) - 1;

    if (want > logBufCap_) return; // a single line larger than the buffer is dropped

    // Protect the ring buffer against concurrent logf() from the timer workloop
    // and registerFrameSource().
    if (logLock_) IOLockLock(logLock_);

    // Drop oldest lines until this one fits.
    while (logBufLen_ + want > logBufCap_) {
        uint8_t *nl = (uint8_t *)memchr(logBuf_, '\n', logBufLen_);
        if (!nl) { logBufLen_ = 0; break; }
        size_t drop = (size_t)(nl - logBuf_) + 1;
        memmove(logBuf_, logBuf_ + drop, logBufLen_ - drop);
        logBufLen_ -= drop;
    }
    memcpy(logBuf_ + logBufLen_, stamped, want);
    logBufLen_ += want;

    if (logLock_) IOLockUnlock(logLock_);
}

void PrimeBridge::logFlush() {
    if (!logEnabled_ || !logBuf_ || logBufLen_ == 0) return;

    // Candidate targets in priority order. macOS lives on the internal disk, so
    // /var/log is writable once the Data volume is up; the USB key's "1401" volume
    // is NOT mounted under macOS (it only mounts when plugged into the build host),
    // so it is a fallback rather than the primary target.
    static const char *const kCandidates[] = {
        "/var/log/PrimeBridge.log",
        "/Volumes/1401/PrimeBridge.log",
    };

    // Choose the first target that opens for append and remember it.
    if (!logReady_) {
        for (size_t i = 0; i < sizeof(kCandidates) / sizeof(kCandidates[0]); i++) {
            vfs_context_t c = vfs_context_create(nullptr);
            if (!c) continue;
            vnode_t vp = nullptr;
            errno_t err = vnode_open(kCandidates[i], FWRITE | O_APPEND | O_CREAT, 0644, 0, &vp, c);
            if (err == 0 && vp) {
                vnode_close(vp, FWRITE, c);
                snprintf(logPath_, sizeof(logPath_), "%s", kCandidates[i]);
                logReady_ = true;
            }
            vfs_context_rele(c);
            if (logReady_) break;
        }
        if (!logReady_) return; // no writable target yet; retry on a later tick
    }

    vfs_context_t ctx = vfs_context_create(nullptr);
    if (!ctx) return;

    vnode_t vp = nullptr;
    int fmode = FWRITE | O_APPEND | O_CREAT;
    errno_t err = vnode_open(logPath_, fmode, 0644, 0, &vp, ctx);
    if (err != 0 || !vp) {
        vfs_context_rele(ctx);
        return; // target became unwritable; buffer retained
    }

    const size_t n = logBufLen_;
    int resid = 0;
    int wr = vn_rdwr(UIO_WRITE, vp, (caddr_t)logBuf_, (int)n, 0,
                     UIO_SYSSPACE, IO_APPEND | IO_SYNC, vfs_context_ucred(ctx), &resid, kernproc);

    vnode_close(vp, FWRITE, ctx);
    vfs_context_rele(ctx);

    if (wr == 0) {
        logBufLen_ = 0;
    }
}

void PrimeBridge::logShutdown() {
    // Final best-effort flush before the object is torn down.
    logFlush();
    if (logBuf_) { IOFree(logBuf_, logBufCap_); logBuf_ = nullptr; }
    logBufCap_ = 0;
    logBufLen_ = 0;
}

IOService *PrimeBridge::probe(IOService *provider, SInt32 *score) {
    int dummy = 0;
    if (PE_parse_boot_argn("-primeoff", &dummy, sizeof(dummy))) return nullptr;  // escape hatch
    return IOService::probe(provider, score);
}

bool PrimeBridge::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    if (!setup(provider)) { cleanup(); IOService::stop(provider); return false; }
    registerService();
    return true;
}

bool PrimeBridge::setup(IOService *provider) {
    igpu_ = OSDynamicCast(IOPCIDevice, provider);
    if (!igpu_) return false;
    igpu_->retain();

    // Allocate the log ring buffer (256 KB) before any other step so failures are logged.
    int dummy = 0;
    if (PE_parse_boot_argn("-primelogoff", &dummy, sizeof(dummy))) {
        // A boot-arg beginning with "-" marks itself merely by existing; the
        // parser never writes the value the caller expects. Treat presence as
        // "off", not "1".
        logEnabled_ = false;
    }
    if (logEnabled_) {
        logBufCap_ = 256 * 1024;
        logBuf_ = static_cast<uint8_t *>(IOMalloc(logBufCap_));
        if (logBuf_) logBufLen_ = 0;
    }
    logf("PrimeBridge " PRIMEBRIDGE_VERSION " starting (log=%d)", logEnabled_ ? 1 : 0);

    if (!fb_.attach(igpu_)) { logf("firmware framebuffer not found or invalid"); return false; }

    // Single rich diagnostic line, all integer (kernel printf has no %f).
    // stride is v_rowBytes octets. stride_px = stride/4 (bpp=32). padding_px =
    // stride_px - width; non-zero means the firmware padded the scanline.
    {
        const uint32_t stridePx = fb_.stride() / 4u;
        logf("FB %ux%u bpp %u stride %u B stride_px %u padding_px %d phys 0x%llx flags 0x%llx BAR0 0x%llx +0x%llx off_in_bar0 0x%llx",
             fb_.width(), fb_.height(), fb_.depth(), fb_.stride(),
             stridePx, (int)stridePx - (int)fb_.width(),
             fb_.physAddr(), fb_.flagBits(), fb_.bar0Phys(), fb_.bar0Len(),
             fb_.physAddr() - fb_.bar0Phys());
    }

    // Registry diagnostics (always readable via: ioreg -l -w0 -r -c PrimeBridge)
    setProperty("GOPWidth",  fb_.width(),  32);
    setProperty("GOPHeight", fb_.height(), 32);
    setProperty("GOPStride", fb_.stride(), 32);
    setProperty("GOPStridePixels", (uint64_t)(fb_.stride() / 4u), 64);
    setProperty("GOPPaddingPixels", (uint64_t)((fb_.stride() / 4u > fb_.width()) ? (fb_.stride() / 4u - fb_.width()) : 0), 64);
    setProperty("GOPDepth",  fb_.depth(),  32);
    setProperty("GOPPhys",   fb_.physAddr(), 64);
    setProperty("GOPFlagBits", fb_.flagBits(), 64);
    setProperty("BAR0Phys",  fb_.bar0Phys(), 64);
    setProperty("BAR0Len",   fb_.bar0Len(),  64);
    setProperty("PrimeBridgeVersion", PRIMEBRIDGE_VERSION);
    setProperty("PrimeMode", "boot-waiting-nvfb");

    // Pick GC/DCN register offsets from the PCI device ID before any probe runs.
    // primegcb/primedcnb override the segment bases; primefb*/primeotg* override
    // individual register offsets, all in Linux's dword units.
    selectRegisterLayout();

    symScanoutInfo_ = OSSymbol::withCStringNoCopy("nvScanoutInfo");

    // Optional boot-args:
    // primetest=1 : SMPTE bars during boot (until NVRMFB is ready)
    // primetest=2 : diagnostic pattern during boot (until NVRMFB is ready)
    // primetest=3 : hold diagnostic pattern for primehold seconds even after NVRMFB is ready
    // primefmt=1  : 8-bit BGRX8888, byte3 forced 0 (DEFAULT; normal GOP/macOS order)
    // primefmt=2  : 8-bit RGBX8888, byte3 forced 0 (R/B swapped)
    // primefmt=3  : raw 32BGRA passthrough (v0.2.0)
    // primefmt=0  : legacy "Mirrored Blue" (v0.3.1)
    // primefmt=4  : comparison-only; "calibrated" against a 1-byte-shifted address
    PE_parse_boot_argn("primetest",   &testMode_,       sizeof(testMode_));
    PE_parse_boot_argn("primemirror", &mirrorExternal_, sizeof(mirrorExternal_));
    PE_parse_boot_argn("primedirect", &directVram_,     sizeof(directVram_));
    PE_parse_boot_argn("primefmt",    &pixelFmt_,       sizeof(pixelFmt_));
    if (pixelFmt_ > 4) pixelFmt_ = 4;
    setProperty("PixelFormat", pixelFmt_, 32);

    // Calibration mire: primecolors=1 cycles raw single-byte probes + checkerboard
    // so the DCN crossbar and stride are measured independently of any packing.
    PE_parse_boot_argn("primecolors", &colorTest_, sizeof(colorTest_));
    if (colorTest_ == 1) {
        colorStage_ = 0;
        colorStageTicks_ = 0;
        fb_.drawCalibration(0);
        logf("calibration mire: stage 0 (byte0)");
    }
    setProperty("ColorTest", colorTest_, 32);

    uint32_t holdS = 60;
    PE_parse_boot_argn("primehold", &holdS, sizeof(holdS));
    if (holdS > 600) holdS = 600;
    if (testMode_) {
        fb_.drawTestPattern(testMode_ == 3 ? 2 : testMode_, pixelFmt_);
        holdTicks_ = holdS * 4;  // 250 ms ticks during boot
        logf("test pattern %u drawn (fmt %u), hold %u s during boot", testMode_, pixelFmt_, holdS);
    }
    setProperty("TestMode", testMode_, 32);

    uint32_t hz = 60;   // default 60 Hz (the internal panel refreshes at 60 Hz)
    PE_parse_boot_argn("primehz", &hz, sizeof(hz));
    if (hz < 24 || hz > 240) hz = 60;
    targetPeriodUs_ = periodUs_ = 1000000 / hz;
    setProperty("TargetHz", hz, 32);

    lock_  = IOLockAlloc();
    logLock_ = IOLockAlloc();
    wl_    = IOWorkLoop::workLoop();
    timer_ = IOTimerEventSource::timerEventSource(this,
               OSMemberFunctionCast(IOTimerEventSource::Action, this, &PrimeBridge::copyTick));
    if (!(lock_ && logLock_ && wl_ && timer_ && wl_->addEventSource(timer_) == kIOReturnSuccess)) return false;

    // Poll every 250 ms during boot until NVRMNVDAFramebuffer + NVAccel are armed
    timer_->setTimeoutMS(250);
    return true;
}

bool PrimeBridge::registerFrameSource(PrimeFrameSource *src) {
    if (!src || !lock_) return false;
    IOLockLock(lock_);
    if (source_) { IOLockUnlock(lock_); return false; }
    src->retain(); source_ = src; lastSeq_ = 0; holdTicks_ = 0;
    IOLockUnlock(lock_);
    timer_->setTimeoutUS(periodUs_);
    setProperty("PrimeMode", "explicit-frame-source");
    logf("frame source registered");
    return true;
}

void PrimeBridge::unregisterFrameSource(PrimeFrameSource *src) {
    IOLockLock(lock_);
    if (source_ == src) { source_->release(); source_ = nullptr; }
    IOLockUnlock(lock_);
}

bool PrimeBridge::findNvidiaFramebuffer() {
    if (nvfb_) return true;
    if (!symScanoutInfo_) return false;

    OSDictionary *match = IOService::serviceMatching("NVRMNVDAFramebuffer");
    if (!match) return false;
    OSIterator *iter = IOService::getMatchingServices(match);
    match->release();
    if (!iter) return false;

    bool found = false;
    while (OSObject *obj = iter->getNextObject()) {
        IOService *cand = OSDynamicCast(IOService, obj);
        if (!cand) continue;

        NVRMVramRequest req {};
        req.version = 2;
        IOReturn rc = cand->callPlatformFunction(symScanoutInfo_, false, &req, nullptr, nullptr, nullptr);
        if (rc == kIOReturnSuccess && req.kva != nullptr &&
            req.width >= 640 && req.height >= 480 && req.pitch >= req.width * 4) {
            cand->retain();
            nvfb_     = cand;
            nvWidth_  = req.width;
            nvHeight_ = req.height;
            nvPitch_  = req.pitch;
            nvPhys_   = req.phys;
            nvSize_   = req.actualSize;

            setProperty("NVFBFound",  true);
            setProperty("NVFBWidth",  nvWidth_,  32);
            setProperty("NVFBHeight", nvHeight_, 32);
            setProperty("NVFBPitch",  nvPitch_,  32);
            setProperty("NVFBPhys",   nvPhys_,   64);
            setProperty("NVFBSize",   nvSize_,   64);
            setProperty("PrimeMode",  "nvfb-found-waiting-iop");
            logf("found NVRMNVDAFramebuffer %ux%u pitch %u phys 0x%llx",
                  nvWidth_, nvHeight_, nvPitch_, nvPhys_);
            found = true;
            break;
        }
    }
    iter->release();
    return found;
}

bool PrimeBridge::attachNvidiaScanout() {
    if (!nvfb_ || !symScanoutInfo_) return false;

    NVRMVramRequest req {};
    req.version = 2;
    IOReturn rc = nvfb_->callPlatformFunction(symScanoutInfo_, false, &req, nullptr, nullptr, nullptr);
    if (rc != kIOReturnSuccess || !req.kva || req.width < 640 || req.height < 480 || req.pitch < req.width * 4) {
        return false;
    }

    nvWidth_   = req.width;
    nvHeight_  = req.height;
    nvPitch_   = req.pitch;
    nvPhys_    = req.phys;
    nvSize_    = req.actualSize;
    origNvKva_ = static_cast<uint8_t *>(req.kva);

    // Find fKva's offset inside NVRMNVDAFramebuffer instead of trusting a constant: NullMoth moved
    // the private layout in 2.9. fKva is the u64 holding req.kva, immediately followed by fPhys, so
    // scanning the object for that pair identifies it uniquely. The store is only made when found.
    const uint8_t *raw = reinterpret_cast<const uint8_t *>(nvfb_);
    size_t kvaOff = 0;
    for (size_t o = 0x40; o + 16 <= 0x1000; o += 8) {
        if (*reinterpret_cast<void * const volatile *>(raw + o) == req.kva &&
            *reinterpret_cast<const volatile uint64_t *>(raw + o + 8) == req.phys) { kvaOff = o; break; }
    }
    offsetsVerified_ = (kvaOff != 0);
    nvfbKvaOff_ = kvaOff;
    setProperty("NVFBOffsetsVerified", offsetsVerified_);
    setProperty("NVFBKvaOffset", (uint64_t)nvfbKvaOff_, 64);
    if (offsetsVerified_)
        logf("NVRMNVDAFramebuffer fKva found at +0x%zx (fPhys at +0x%zx)", nvfbKvaOff_, nvfbKvaOff_ + 8);

    if (offsetsVerified_ && directVram_ == 0) {
        // Never shrink: NVRMFB's switchMode keeps fKva (= staging_), so after a mode change NVAccel
        // writes the new mode's frames here before tickNullMoth's geometry check sees it.
        // shortcut: a mode larger than the first one attached still overflows until that check
        // (only nvfbhidpi=1 or a bigger external monitor allow one), upgrade: size from NVRMFB's
        // scanout allocation (fMemBytes, not in the nvScanoutInfo ABI).
        const size_t need = (size_t)nvPitch_ * nvHeight_;
        if (!staging_ || need > stagingSize_) {
            // 0x2d8 already points back at VRAM here, so the old buffers are unreferenced.
            if (staging_) { IOFreeAligned(staging_, stagingSize_); staging_ = nullptr; }
            if (shadow_)  { IOFreeAligned(shadow_, stagingSize_);  shadow_  = nullptr; }
            stagingSize_ = need;
            staging_ = static_cast<uint8_t *>(IOMallocAligned(stagingSize_, PAGE_SIZE));
            shadow_  = static_cast<uint8_t *>(IOMallocAligned(stagingSize_, PAGE_SIZE));
        }
        if (staging_) {
            memcpy(staging_, origNvKva_, need);
            if (shadow_) memset(shadow_, 0, need);
            // Redirect NVRMNVDAFramebuffer's fKva (0x2d8) to cached system RAM staging_, so
            // NVAccel's performTransaction deposits frames into cached RAM instead of VRAM.
            __asm__ volatile("sfence" ::: "memory");
            *reinterpret_cast<void * volatile *>(reinterpret_cast<uint8_t *>(nvfb_) + nvfbKvaOff_) = staging_;
            __asm__ volatile("sfence" ::: "memory");
            setProperty("PrimeMode", "staging-redirect-ce-8bit");
            logf("redirected NVRMNVDAFramebuffer scanout kva to cached staging (%llu bytes)",
                  (unsigned long long)stagingSize_);
        } else {
            setProperty("PrimeMode", "direct-vram-alloc-failed");
        }
    } else {
        setProperty("PrimeMode", "direct-vram-fallback");
    }

    redirected_ = true;
    firstFrame_ = true;
    return true;
}

// Pick the BAR5 register layout for this iGPU. The 680M keeps the values this
// project shipped with; the Phoenix/Hawk Point family (GC 11.0.x) uses the GC
// 11.0 offsets. Every value can be overridden for bring-up: the GC/DCN segment
// bases come from the APU's IP discovery table, not from code, so they are the
// one part that cannot be read from a public register header.
void PrimeBridge::selectRegisterLayout() {
    const uint16_t vendor = igpu_->configRead16(kIOPCIConfigVendorID);
    const uint16_t device = igpu_->configRead16(kIOPCIConfigDeviceID);
    const IgpuRegLayout &layout = (device == 0x1681) ? kLayoutRembrandt
                               : (device == 0x1900 || device == 0x1901) ? kLayoutHawkPoint
                               : kLayoutPhoenix;

    uint32_t gcBase    = layout.gcBase;
    uint32_t dcnBase   = layout.dcnBase;
    uint32_t fbOffset  = layout.fbOffset;
    uint32_t fbLocBase = layout.fbLocBase;
    uint32_t fbLocTop  = layout.fbLocTop;
    uint32_t otgCtl    = layout.otg0Ctl;
    uint32_t otgHTotal = layout.otg0HTotal;
    uint32_t otgVTotal = layout.otg0VTotal;
    uint32_t otgFrames = layout.otg0Frames;
    uint32_t otgStride = layout.otgStride;

    PE_parse_boot_argn("primegcb",    &gcBase,    sizeof(gcBase));
    PE_parse_boot_argn("primedcnb",   &dcnBase,   sizeof(dcnBase));
    PE_parse_boot_argn("primefbofs",  &fbOffset,  sizeof(fbOffset));
    PE_parse_boot_argn("primefbbase", &fbLocBase, sizeof(fbLocBase));
    PE_parse_boot_argn("primefbtop",  &fbLocTop,  sizeof(fbLocTop));
    PE_parse_boot_argn("primeotgctl", &otgCtl,    sizeof(otgCtl));
    PE_parse_boot_argn("primeotgh",   &otgHTotal, sizeof(otgHTotal));
    PE_parse_boot_argn("primeotgv",   &otgVTotal, sizeof(otgVTotal));
    PE_parse_boot_argn("primeotgf",   &otgFrames, sizeof(otgFrames));
    PE_parse_boot_argn("primeotgs",   &otgStride, sizeof(otgStride));

    // Compute in 64 bits so a bogus boot-arg cannot wrap into a small offset.
    auto regByte = [](uint32_t base, uint32_t reg) -> uint64_t {
        return ((uint64_t)base + reg) * 4;
    };
    const uint64_t fbOffsetB  = regByte(gcBase, fbOffset);
    const uint64_t fbLocBaseB = regByte(gcBase, fbLocBase);
    const uint64_t fbLocTopB  = regByte(gcBase, fbLocTop);
    const uint64_t otgCtlB    = regByte(dcnBase, otgCtl);
    const uint64_t otgHTotalB = regByte(dcnBase, otgHTotal);
    const uint64_t otgVTotalB = regByte(dcnBase, otgVTotal);
    const uint64_t otgFramesB = regByte(dcnBase, otgFrames);
    const uint64_t otgStrideB = (uint64_t)otgStride * 4;
    const uint64_t lastOtgB   = otgFramesB + 3 * otgStrideB;

    const bool inRange = fbOffsetB  + 4 <= kBar5MapLen &&
                         fbLocBaseB + 4 <= kBar5MapLen &&
                         fbLocTopB  + 4 <= kBar5MapLen &&
                         otgCtlB    + 4 <= kBar5MapLen &&
                         otgHTotalB + 4 <= kBar5MapLen &&
                         otgVTotalB + 4 <= kBar5MapLen &&
                         lastOtgB   + 4 <= kBar5MapLen;
    regLayoutValid_ = inRange;

    gcBase_  = gcBase;
    dcnBase_ = dcnBase;
    if (inRange) {
        regFbOffset_  = (uint32_t)fbOffsetB;
        regFbLocBase_ = (uint32_t)fbLocBaseB;
        regFbLocTop_  = (uint32_t)fbLocTopB;
        regOtg0Ctl_    = (uint32_t)otgCtlB;
        regOtg0HTotal_ = (uint32_t)otgHTotalB;
        regOtg0VTotal_ = (uint32_t)otgVTotalB;
        regOtg0Frames_ = (uint32_t)otgFramesB;
        otgStride_     = (uint32_t)otgStrideB;
    }

    setProperty("RegisterLayout", layout.name);
    setProperty("GCBase", gcBase_, 32);
    setProperty("DCNBase", dcnBase_, 32);
    setProperty("RegFbOffset", (uint64_t)fbOffsetB, 64);
    setProperty("RegOtg0Ctl", (uint64_t)otgCtlB, 64);
    logf("iGPU %04x:%04x layout %s: GC base 0x%x DCN base 0x%x, FB 0x%llx/0x%llx/0x%llx, OTG 0x%llx/0x%llx/0x%llx/0x%llx stride 0x%llx -> %s",
         vendor, device, layout.name, gcBase, dcnBase,
         fbOffsetB, fbLocBaseB, fbLocTopB,
         otgCtlB, otgHTotalB, otgVTotalB, otgFramesB, otgStrideB,
         inRange ? "in BAR5" : "OUT OF BAR5 RANGE");
}

// Read-only groundwork for writing the 680M's memory directly instead of through BAR0, as Linux amdgpu
// does on x86 APUs (gmc_v10_0.c): where that memory sits in system RAM, whether BAR0 and that address
// show the same pixels, and the panel timing. Never writes; runs once, on a still screen.
void PrimeBridge::setPanelBrightness(uint32_t pct)
{
    if (pct > 100) pct = 100;
    if (!regs_ && igpu_) {
        IODeviceMemory *bar5 = igpu_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
        if (!mmio_ && bar5 && bar5->getLength() >= kBar5MapLen)
            mmio_ = bar5->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache, 0, kBar5MapLen);
        if (mmio_) regs_ = reinterpret_cast<volatile uint32_t *>(mmio_->getVirtualAddress());
    }
    if (!regs_ || !dcnBase_) return;
    const uint64_t boff = ((uint64_t)dcnBase_ + kBlPwmCntl) * 4;
    if (boff + 4 > kBar5MapLen) return;
    const uint32_t v = regs_[boff / 4];
    const uint32_t duty = (uint32_t)((uint64_t)pct * 65535u / 100u);
    if ((v & 0xFFFFu) == duty) return;   // unchanged: skip the write and the log line
    regs_[boff / 4] = (v & ~0xFFFFu) | duty;
    logf("brightness: panel %u%% (PWRSEQ0_BL_PWM_CNTL 0x%08x -> 0x%08x)", pct, v, regs_[boff / 4]);
}

IOReturn PrimeBridge::callPlatformFunction(const OSSymbol *fn, bool wait,
                                           void *p1, void *p2, void *p3, void *p4)
{
    if (fn && fn->isEqualTo("NVRMBacklight")) {
        // NullMoth's backlight handler: p2 == NULL reads the level, p2 != NULL sets p1 (percent).
        if (!p2) {
            if (p1) *(UInt32 *)p1 = 80;
            return kIOReturnSuccess;
        }
        setPanelBrightness(p1 ? *(UInt32 *)p1 : 80);
        return kIOReturnSuccess;
    }
    return IOService::callPlatformFunction(fn, wait, p1, p2, p3, p4);
}

void PrimeBridge::probeIgpu() {
    IODeviceMemory *bar5 = igpu_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    if (!mmio_ && bar5 && bar5->getLength() >= 0x80000)
        mmio_ = bar5->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache, 0, 0x80000);
    if (!mmio_) { logf("igpu probe: BAR5 not mapped"); return; }
    if (!regLayoutValid_) { logf("igpu probe: register layout invalid, skipping"); return; }
    regs_ = reinterpret_cast<volatile uint32_t *>(mmio_->getVirtualAddress());

    // primebright=N (0..100): scale the panel backlight PWM duty, as a direct test of whether the
    // internal panel's backlight is driven by the DCN's PWRSEQ0_BL_PWM_CNTL (see kBlPwmCntl).
    {
        uint32_t bright = 0xFFFFFFFFu;
        if (PE_parse_boot_argn("primebright", &bright, sizeof bright) && bright <= 100) {
            const uint64_t boff = ((uint64_t)dcnBase_ + kBlPwmCntl) * 4;
            if (boff + 4 <= kBar5MapLen) {
                const uint32_t v = regs_[boff / 4];
                const uint32_t cur = v & 0xFFFFu;
                const uint32_t nd  = (uint32_t)((uint64_t)cur * bright / 100u);
                regs_[boff / 4] = (v & ~0xFFFFu) | nd;
                logf("brightness: PWRSEQ0_BL_PWM_CNTL 0x%08x duty %u -> %u (%u%%) at 0x%llx",
                     v, cur, nd, bright, boff);
            } else {
                logf("brightness: backlight register outside BAR5");
            }
        }
    }

    const uint32_t off = regs_[regFbOffset_ / 4], base = regs_[regFbLocBase_ / 4], top = regs_[regFbLocTop_ / 4];
    uint64_t vramPhys = (uint64_t)(off & 0xFFFFFFu) << 24;
    uint64_t vramSize = (top & 0xFFFFFFu) >= (base & 0xFFFFFFu)
                      ? (uint64_t)((top & 0xFFFFFFu) - (base & 0xFFFFFFu) + 1) << 24 : 0;
    // GC 10.x (Rembrandt) keeps the window in the GC block and the reads above work.
    // On GC 11 (Phoenix/Hawk Point) those registers read 0 because the window lives in
    // the MMHUB block. Try the known MMHUB segment bases; the MATCH check below rejects
    // a wrong pick, so this only ever adds a chance at direct mode.
    if (!vramPhys || !vramSize) {
        static constexpr uint32_t kMmBases[] = { 0x1A000, 0x13200 };
        for (uint32_t mb : kMmBases) {
            const uint64_t bOff = ((uint64_t)mb + kMmFbLocBase) * 4;
            const uint64_t tOff = ((uint64_t)mb + kMmFbLocTop) * 4;
            if (bOff + 4 > kBar5MapLen || tOff + 4 > kBar5MapLen) continue;
            const uint32_t mbBase = regs_[bOff / 4], mbTop = regs_[tOff / 4];
            if (!(mbBase & 0xFFFFFFu) || (mbTop & 0xFFFFFFu) < (mbBase & 0xFFFFFFu)) continue;
            vramPhys = (uint64_t)(mbBase & 0xFFFFFFu) << 24;
            vramSize = (uint64_t)((mbTop & 0xFFFFFFu) - (mbBase & 0xFFFFFFu) + 1) << 24;
            logf("igpu probe: MMHUB seg 0x%x MMMC_VM_FB_LOCATION 0x%08x..0x%08x -> VRAM 0x%llx, %llu MB",
                 mb, mbBase, mbTop, vramPhys, vramSize >> 20);
            break;
        }
    }
    const uint64_t gopOff = fb_.physAddr() - fb_.bar0Phys(), len = (uint64_t)fb_.stride() * fb_.height();
    logf("igpu probe: FB_OFFSET 0x%08x FB_LOCATION 0x%08x..0x%08x -> VRAM at phys 0x%llx, %llu MB; GOP at VRAM +0x%llx",
         off, base, top, vramPhys, vramSize >> 20, gopOff);

    IOMemoryMap *ram = nullptr;
    if (off != 0xFFFFFFFFu && vramPhys && gopOff + len <= vramSize) {
        // Default cache mode: uncached if this is reserved memory as expected, the kernel's own mode if not.
        if (IODeviceMemory *md = IODeviceMemory::withRange(vramPhys + gopOff, len)) {
            ram = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapDefaultCache | kIOMapReadOnly);
            md->release();
        }
    }
    // What PrimeBridge last wrote (shadow_, packed like blitRow's default format) is the reference: BAR0 reads
    // are only logged, since the 0.4.6 probe read all-black rows through BAR0 on a lit desktop.
    const bool haveRef = shadow_ && pixelFmt_ == 1 && nvPitch_ == fb_.stride() && nvHeight_ >= fb_.height();
    if (ram && haveRef) {
        uint32_t px = 0, diffBar = 0, diffRef = 0, litBar = 0, litRef = 0;
        for (uint32_t k = 0; k < 32; k++) {
            const uint64_t row = (uint64_t)((2 * k + 1) * fb_.height() / 64) * fb_.stride();
            const volatile uint32_t *a = reinterpret_cast<const volatile uint32_t *>(fb_.base() + row);
            const volatile uint32_t *b = reinterpret_cast<const volatile uint32_t *>(ram->getVirtualAddress() + row);
            const uint32_t *s = reinterpret_cast<const uint32_t *>(shadow_ + row);
            for (uint32_t x = 0; x < fb_.width(); x++, px++) {
                const uint32_t pa = a[x], pb = b[x], ps = s[x] & 0x00FFFFFFu;
                if (pa != pb) diffBar++;
                if (ps != pb) diffRef++;
                if (pa & 0xFFFFFFu) litBar++;
                if (ps) litRef++;
            }
        }
        ram->release();
        const bool match = !diffRef && litRef >= 1000;
        logf("igpu probe: phys 0x%llx, %u px: %u differ from what we wrote (%u lit), %u from BAR0 (%u lit) -> %s",
             vramPhys + gopOff, px, diffRef, litRef, diffBar, litBar,
             diffRef ? "NO MATCH" : match ? "MATCH" : "inconclusive (screen too dark)");
        if (diffRef || match) probed_ = true;   // a dark screen is tried again on a later still screen
        uint32_t carve = 1;
        PE_parse_boot_argn("primecarve", &carve, sizeof(carve));
        if (match && carve && shadow_) chooseWriteMapping(vramPhys + gopOff);
        else if (carve) {
            // The VRAM address did not verify. Direct mode does not actually need it: the panel
            // is scanned out of BAR0, so if BAR0 write-combined is fast we can still point the
            // scanout straight at the GOP framebuffer.
            if (fb_.bar0Len() && fb_.physAddr() >= fb_.bar0Phys() &&
                fb_.physAddr() + len <= fb_.bar0Phys() + fb_.bar0Len())
                chooseWriteMapping(fb_.physAddr());
            else
                logf("igpu probe: GOP 0x%llx outside BAR0 [0x%llx +0x%llx], no BAR0 direct mode",
                     fb_.physAddr(), fb_.bar0Phys(), fb_.bar0Len());
        }
    } else {
        if (ram) ram->release();
        probed_ = true;
        logf("igpu probe: no comparison (VRAM address implausible or not mappable, or no reference frame)");
        // Direct mode does not actually need the VRAM carveout address: the panel is scanned
        // out of BAR0, so if BAR0 write-combined is fast enough we can point the scanout
        // straight at the GOP framebuffer. Benchmark BAR0 against BAR0-WC only.
        uint32_t carve = 1;
        PE_parse_boot_argn("primecarve", &carve, sizeof(carve));
        if (carve && shadow_ && fb_.bar0Len() && fb_.physAddr() >= fb_.bar0Phys() &&
            fb_.physAddr() + len <= fb_.bar0Phys() + fb_.bar0Len()) {
            chooseWriteMapping(fb_.physAddr());
        } else if (carve) {
            logf("igpu probe: GOP 0x%llx outside BAR0 [0x%llx +0x%llx], no BAR0 direct mode",
                 fb_.physAddr(), fb_.bar0Phys(), fb_.bar0Len());
        }
    }

    // Panel timing: the OTG the GOP left running drives the panel.
    for (uint32_t i = 0; i < 4 && !otgFrameReg_; i++) {
        if (!(regs_[(regOtg0Ctl_ + i * otgStride_) / 4] & 1u)) continue;
        otgFrameReg_ = regOtg0Frames_ + i * otgStride_;
        otgFrames_ = regs_[otgFrameReg_ / 4];
        otgAbs_ = mach_absolute_time();
        logf("igpu probe: OTG%u running, h_total %u v_total %u", i,
             (regs_[(regOtg0HTotal_ + i * otgStride_) / 4] & 0x7FFFu) + 1,
             (regs_[(regOtg0VTotal_ + i * otgStride_) / 4] & 0x7FFFu) + 1);
    }
    if (!otgFrameReg_) logf("igpu probe: no OTG running");
}

// Times rewriting 64 rows with the pixels already on screen (invisible) through BAR0 as attach() mapped it,
// BAR0 write-combined, and the 680M's VRAM write-combined, then keeps the fastest. Runs on a still tick.
void PrimeBridge::chooseWriteMapping(uint64_t vramGop) {
    uint32_t lo = 0, hi = 0;
    if (rdmsr_carefully(0x277, &lo, &hi) == 0)   // IA32_PAT: XNU's write-combined PTEs use entry PA6
        logf("igpu writes: PAT 0x%08x%08x (PA6 %s)", hi, lo, ((hi >> 16) & 7u) == 1 ? "WC" : "not WC");
    auto bench = [this]() -> uint64_t {
        const uint64_t t0 = mach_absolute_time();
        for (uint32_t k = 0; k < 64; k++) {
            const uint32_t y = (2 * k + 1) * fb_.height() / 128;
            fb_.blitRow(y, reinterpret_cast<const uint32_t *>(shadow_ + (uint64_t)y * nvPitch_), fb_.width(), pixelFmt_);
        }
        __builtin_ia32_sfence();
        uint64_t ns;
        absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
        return ns / 64;
    };
    const uint64_t tBar = bench();
    const uint64_t tBarWC = fb_.mapWriteCombined(fb_.physAddr()) ? bench() : ~0ULL;
    const uint64_t tVram = fb_.mapWriteCombined(vramGop) ? bench() : ~0ULL;
    const bool vram = tVram < tBarWC && tVram < tBar;
    if (!vram) fb_.mapWriteCombined(fb_.physAddr());   // never worse than attach()'s mapping: both at worst uncached
    const uint64_t best = vram ? tVram : (tBarWC < tBar ? tBarWC : tBar);
    fastWrites_ = best < 3000;   // >= 2.5 GB/s: only then can NVAccel afford to write every frame to the panel
    setProperty("PrimeMode", vram ? "staging-redirect-carveout" : "staging-redirect-ce-8bit");
    logf("igpu writes: ns per row: BAR0 %llu, BAR0 write-combined %llu, VRAM write-combined %llu -> %s%s",
         tBar, tBarWC, tVram, vram ? "VRAM at phys" : "BAR0", fastWrites_ ? ", fast" : "");
}

void PrimeBridge::tickNullMoth(IOTimerEventSource *t) {
    // Calibration mire (primecolors=1): test the iGPU scanout ALONE. Cycle the 6
    // raw-probe stages at ~2 s each and never run frame copy, so the DCN crossbar
    // and stride are measured with zero interference from NVRMFB or packing. This
    // works with or without an external monitor (GOP is always scanned by the DCN).
    if (colorTest_ == 1) {
        if (++colorStageTicks_ >= 8) {   // ~250 ms tick => ~2 s per stage
            colorStageTicks_ = 0;
            colorStage_ = (colorStage_ + 1) % 6;
            fb_.drawCalibration(colorStage_);
            logf("calibration mire: stage %u", colorStage_);
        }
        if ((++logTick_ & 15u) == 0) logFlush();
        t->setTimeoutMS(250);
        return;
    }

    // Flush the log on a cadence independent of the frame-copy statTick_, so the
    // boot-waiting and iop-arming phases (which may last tens of seconds) are
    // persisted too, not just the post-redirect steady state.
    if ((++logTick_ & 15u) == 0) logFlush();

    if (!nvfb_) {
        findNvidiaFramebuffer();
        if (!nvfb_) {
            if (testMode_ && holdTicks_ > 0) {
                holdTicks_--;
                fb_.drawTestPattern(testMode_ == 3 ? 2 : testMode_, pixelFmt_);
            }
            t->setTimeoutMS(250);
            return;
        }
    }

    if (!redirected_) {
        // Wait until NVRM::autoGo has armed NVAccel's display pipe (debug.nvaccel_iop == 1)
        int iopArmed = 0;
        size_t iopSz = sizeof(iopArmed);
        sysctlbyname("debug.nvaccel_iop", &iopArmed, &iopSz, nullptr, 0);

        if (iopArmed != 1 || (testMode_ == 3 && holdTicks_ > 0)) {
            if (testMode_ && holdTicks_ > 0) {
                holdTicks_--;
                fb_.drawTestPattern(testMode_ == 3 ? 2 : testMode_, pixelFmt_);
            }
            t->setTimeoutMS(200);
            return;
        }

        // Ensure debug.nvaccel_iop_flip = 0 so NVAccel uses the copy path into staging_.
        // (Zero-copy flip is incompatible with the kva redirect — see nullmoth-internals.)
        int flipVal = 0;
        size_t flipSz = sizeof(flipVal);
        if (sysctlbyname("debug.nvaccel_iop_flip", &flipVal, &flipSz, nullptr, 0) == 0 && flipVal != 0) {
            int zero = 0;
            sysctlbyname("debug.nvaccel_iop_flip", nullptr, nullptr, &zero, sizeof(zero));
            flipCleared_++;
            setProperty("PrimeFlipCleared", flipCleared_, 32);
        }

        if (!attachNvidiaScanout()) {
            t->setTimeoutMS(200);
            return;
        }
    }

    const uint8_t *srcBuf = staging_ ? staging_ : origNvKva_;
    if (!srcBuf) {
        t->setTimeoutMS(100);
        return;
    }

    // Re-verify the scanout geometry ~1/s to detect a mode change, which would
    // otherwise make the copy walk stale dimensions (and overflow if it grew).
    if ((statTick_ & 63u) == 0 && redirected_) {
        NVRMVramRequest req {};
        req.version = 2;
        if (nvfb_->callPlatformFunction(symScanoutInfo_, false, &req, nullptr, nullptr, nullptr) == kIOReturnSuccess) {
            const void *ours = directKva_ ? (const void *)fb_.base() : (const void *)(staging_ ? staging_ : origNvKva_);
            if (req.kva && (req.width != nvWidth_ || req.height != nvHeight_ || req.pitch != nvPitch_ ||
                            req.kva != ours)) {
                logf("scanout geometry changed (or kva no longer staging): %ux%u p%u -> %ux%u p%u; restoring orig kva and re-attaching",
                     nvWidth_, nvHeight_, nvPitch_, req.width, req.height, req.pitch);
                // Restore NVRMNVDAFramebuffer's fKva back to the original VRAM
                // pointer BEFORE declaring the redirect dead, otherwise the next
                // attachNvidiaScanout() verification sees the stale staging_ and
                // can never re-match (self-deadlock: frozen frame + reset loop).
                if (nvfb_ && origNvKva_ && offsetsVerified_) {
                    __asm__ volatile("sfence" ::: "memory");
                    *reinterpret_cast<void * volatile *>(reinterpret_cast<uint8_t *>(nvfb_) + nvfbKvaOff_) = origNvKva_;
                    __asm__ volatile("sfence" ::: "memory");
                }
                redirected_ = false;
                directKva_ = false;
                t->setTimeoutMS(200);
                return;   // re-attach on a later tick via the !redirected_ path
            }
        }
    }

    const uint32_t w = nvWidth_  < fb_.width()  ? nvWidth_  : fb_.width();
    const uint32_t h = nvHeight_ < fb_.height() ? nvHeight_ : fb_.height();
    const size_t   rowBytes = (size_t)w * 4;

    bool anyRowChanged = false;

    // Diff frames in cached system RAM on every tick (primehz, default 60 Hz).
    // NOTE: NVAccel's performTransaction writes staging_ on its own workloop while this
    // reads it — a torn frame can slip through. Tearing is inherent to the redirect;
    // acceptable vs. the uncached-VRAM alternative.
    const uint64_t workStart = mach_absolute_time();
    if (!directKva_) {   // direct: NVAccel already wrote the frame into the GOP
        uint8_t *mirrorDst = (staging_ && mirrorExternal_) ? origNvKva_ : nullptr;
        for (uint32_t y = 0; y < h; y++) {
            const uint32_t *srcRow = reinterpret_cast<const uint32_t *>(srcBuf + (uint64_t)y * nvPitch_);
            uint32_t       *shdRow = shadow_ ? reinterpret_cast<uint32_t *>(shadow_ + (uint64_t)y * nvPitch_) : nullptr;

            if (firstFrame_ || !shdRow || memcmp(srcRow, shdRow, rowBytes) != 0) {
                if (shdRow) memcpy(shdRow, srcRow, rowBytes);
                if (mirrorDst) memcpy(mirrorDst + (uint64_t)y * nvPitch_, srcRow, rowBytes);
                const uint64_t b = mach_absolute_time();
                fb_.blitRow(y, srcRow, w, pixelFmt_);
                statBlit_ += mach_absolute_time() - b;
                statRows_++;
                anyRowChanged = true;
            }
        }
        if (anyRowChanged) {
            firstFrame_ = false;
            framesCopied_++;
        }
    }

    if (anyRowChanged) __builtin_ia32_sfence();
    const uint64_t work = mach_absolute_time() - workStart;
    statWork_ += work;
    if (work > statWorkMax_) statWorkMax_ = work;

    // Adaptive frequency: when nothing changed, idleTicks_ climbs; after a short
    // idle the loop drops from the target rate to 20 Hz and only restores it when
    // a change is seen again. Keeps the CPU able to sleep when the desktop is still.
    if (anyRowChanged) {
        idleTicks_ = 0;
        if (lowFreqMode_) { lowFreqMode_ = false; }
    } else {
        idleTicks_++;
    }
    // Idle throttling removed: dropping the copy to 20 Hz on a still screen read as
    // "stuck at 20 Hz" whenever content changed slowly. Keep the steady target rate.
    (void)idleTicks_;

    // Probe the iGPU as soon as the screen has been still, then switch to direct the moment the
    // write benchmark says it is fast. Both used to wait for the 720-tick statistics window, which
    // held the direct-mode switch back by seconds after the redirect attached.
    if (!probed_ && !anyRowChanged && (++probeTick_ & 15u) == 0u) probeIgpu();
    if (fastWrites_ && !directKva_ && redirected_ && staging_ && offsetsVerified_ && pixelFmt_ == 1 && !mirrorExternal_ &&
        nvWidth_ == fb_.width() && nvHeight_ == fb_.height() && nvPitch_ == fb_.stride()) {
        __asm__ volatile("sfence" ::: "memory");
        *reinterpret_cast<void * volatile *>(reinterpret_cast<uint8_t *>(nvfb_) + nvfbKvaOff_) = fb_.base();
        __asm__ volatile("sfence" ::: "memory");
        directKva_ = true;
        setProperty("PrimeMode", "direct-kva");
        logf("NVAccel now writes its frames straight into the panel's buffer (no staging copy, no diff)");
    }

    // Update IORegistry diagnostics + flush the log file + log effective rate ~5 s.
    if (++statTick_ >= 720) {
        statTick_ = 0;
        setProperty("FramesCopied", framesCopied_, 64);
        setProperty("EffectiveRateHz", (uint64_t)(1000000 / (lowFreqMode_ ? 50000u : periodUs_)), 64);
        // Per 720 ticks: copy loop time (avg, max), of which writes to the GOP, and rows written per tick.
        uint64_t workNs, maxNs, blitNs;
        absolutetime_to_nanoseconds(statWork_, &workNs);
        absolutetime_to_nanoseconds(statWorkMax_, &maxNs);
        absolutetime_to_nanoseconds(statBlit_, &blitNs);
        // Panel refresh over the same window, from the 680M's own frame counter (found by probeIgpu).
        uint32_t panelHz100 = 0;
        if (otgFrameReg_) {
            const uint32_t f = regs_[otgFrameReg_ / 4];
            const uint64_t at = mach_absolute_time();
            uint64_t ns;
            absolutetime_to_nanoseconds(at - otgAbs_, &ns);
            panelHz100 = hz100(otgFrames_, f, ns);
            otgFrames_ = f;
            otgAbs_ = at;
        }
        logf("rate-hint: %u Hz%s, work avg %llu us max %llu us, GOP writes avg %llu us, rows/tick %llu, panel %u.%02u Hz%s",
             1000000 / (lowFreqMode_ ? 50000u : periodUs_), lowFreqMode_ ? " (idle)" : "",
             workNs / 720000, maxNs / 1000, blitNs / 720000, statRows_ / 720, panelHz100 / 100, panelHz100 % 100,
             directKva_ ? ", direct" : "");
        statWork_ = statWorkMax_ = statBlit_ = statRows_ = 0;
        logFlush();
    }

    // Schedule the next tick at the effective rate (20 Hz when idle) on a fixed cadence: a delay
    // counted from the end of the work made each tick last the period plus the work (52 Hz, not 60).
    uint64_t period, now = mach_absolute_time();
    clock_interval_to_absolutetime_interval(lowFreqMode_ ? 50000u : periodUs_, kMicrosecondScale, &period);
    nextTickAbs_ = nextTickAbs_ + period > now ? nextTickAbs_ + period : now + period;   // late: restart
    t->wakeAtTime(nextTickAbs_);
}

void PrimeBridge::copyTick(IOTimerEventSource *t) {
    IOLockLock(lock_);
    PrimeFrameSource *src = source_;
    if (src) src->retain();
    IOLockUnlock(lock_);

    if (!src) {
        tickNullMoth(t);
        return;
    }

    PrimeFrame f {};
    if (src->acquireFrame(&f)) {
        if (f.seq != lastSeq_) {
            if (src->gpuCopyTo(f, fb_.physAddr(), fb_.stride()) != kIOReturnSuccess)
                fb_.blitFromCPU(f, pixelFmt_);
            lastSeq_ = f.seq;
            framesCopied_++;
        }
        src->releaseFrame(&f);
    }
    src->release();
    t->setTimeoutUS(periodUs_);
}

void PrimeBridge::cleanup() {
    if (timer_) {
        timer_->cancelTimeout();
        if (wl_) wl_->removeEventSource(timer_);
        timer_->release(); timer_ = nullptr;
    }
    if (wl_) { wl_->release(); wl_ = nullptr; }

    // Restore original NVRMNVDAFramebuffer scanout pointer before freeing staging_.
    if (nvfb_ && staging_ && origNvKva_ && offsetsVerified_) {
        __asm__ volatile("sfence" ::: "memory");
        *reinterpret_cast<void * volatile *>(reinterpret_cast<uint8_t *>(nvfb_) + nvfbKvaOff_) = origNvKva_;
        __asm__ volatile("sfence" ::: "memory");
    }
    // NVAccel's performTransaction may still be mid-write into staging_ on its own
    // workloop. Restoring the pointer does not stop that. Wait one period before
    // freeing so an in-flight copy lands in the original VRAM, not freed memory.
    // (Hot unload remains inherently racy; this only narrows the window.)
    __asm__ volatile("sfence" ::: "memory");
    if (staging_) { IOSleep(50); __asm__ volatile("sfence" ::: "memory"); }
    if (staging_) { IOFreeAligned(staging_, stagingSize_); staging_ = nullptr; }
    if (shadow_)  { IOFreeAligned(shadow_, stagingSize_);  shadow_  = nullptr; }
    stagingSize_ = 0;

    if (nvfb_) { nvfb_->release(); nvfb_ = nullptr; }
    if (symScanoutInfo_) { symScanoutInfo_->release(); symScanoutInfo_ = nullptr; }

    if (lock_) {
        IOLockLock(lock_);
        if (source_) { source_->release(); source_ = nullptr; }
        IOLockUnlock(lock_);
        IOLockFree(lock_); lock_ = nullptr;
    }
    if (logLock_) { IOLockFree(logLock_); logLock_ = nullptr; }
    logShutdown();
    if (mmio_) { mmio_->release(); mmio_ = nullptr; regs_ = nullptr; otgFrameReg_ = 0; }
    fb_.detach();
    if (igpu_) { igpu_->release(); igpu_ = nullptr; }
}

void PrimeBridge::stop(IOService *provider) { cleanup(); IOService::stop(provider); }
