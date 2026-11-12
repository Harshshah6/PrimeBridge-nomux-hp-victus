# Prompt: add macOS brightness control to a PrimeBridge-style setup

Copy everything below to an AI assistant working on that machine.

---

You are working on a Hackintosh with two cooperating kexts:

- **NullMoth's `NVRMFB`** (the NVIDIA framebuffer), patched with a `nvfbforce` change that,
  when nothing is plugged into the NVIDIA GPU, forces one HDMI connector "connected" with a
  built-in fake EDID (`kForcedEdid`). That fake head exists only so NVAccel runs and macOS
  composites on the NVIDIA GPU.
- **A bridge kext (this project, `PrimeBridge`)** that reuses the framebuffer the firmware (GOP)
  left on the **AMD iGPU** — which macOS has no driver for — and copies/points NVAccel's frames
  at it. It already maps the iGPU's BAR5 (MMIO) for its own probing.

The internal panel is physically on the AMD iGPU, so its **backlight is on the iGPU**, not the
NVIDIA GPU. Because macOS sees a fake HDMI display on the NVIDIA GPU, there is (a) no brightness
slider, because macOS only offers one for a display it considers *built-in*, and (b) NullMoth's
brightness handler was never created, because it asks NVRM for a backlight and a fake HDMI has none.

Goal: make the macOS brightness slider (and brightness keys) actually dim the internal panel.

## Step 0 — establish facts before writing code

1. Confirm the bridge kext's name (here it is `com.myvo.PrimeBridge`, service class `PrimeBridge`)
   and that it maps the iGPU's BAR5. Find where it computes the DCN segment base (on AMD APUs
   the DCN/DMU block is segment 2, commonly `0x34C0` dwords; it may come from IP discovery).
2. Find the panel backlight register for the iGPU's **DCN version** in Linux's headers:
   `drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_<ver>_offset.h` and `..._sh_mask.h`.
   The relevant registers are `PWRSEQ0_BL_PWM_CNTL`, `PWRSEQ0_BL_PWM_CNTL2`,
   `PWRSEQ0_BL_PWM_PERIOD_CNTL`, all with `BASE_IDX 2` (the DCN segment).
   Duty cycle is `BL_ACTIVE_INT_FRAC_CNT` (bits [15:0]); `BL_PWM_EN` is bit 31;
   `BL_PWM_FRACTIONAL_EN` is bit 30. For DCN 3.1.4 `PWRSEQ0_BL_PWM_CNTL = 0x2f19`, so the bare
   BAR5 byte offset is `(dcn_seg2_base + 0x2f19) * 4` (e.g. `(0x34C0 + 0x2f19) * 4 = 0x18F64`).

## Step 1 — prove the register path FIRST (before touching the slider)

In the bridge kext, add a one-shot boot-arg, e.g. `primebright=N` (0..100), that on probe,
after BAR5 is mapped, does a read-modify-write of `PWRSEQ0_BL_PWM_CNTL`:

```cpp
uint32_t v = regs[off/4];
uint32_t duty = (uint32_t)((uint64_t)pct * 65535u / 100u);
regs[off/4] = (v & ~0xFFFFu) | duty;      // preserve enable/fractional bits
```

Reboot with `primebright=30`. **If the panel visibly dims, the panel is driven by the DCN PWM
and the rest of this plan will work.** If nothing changes, the backlight is elsewhere (eDP AUX
DPCD `0x722`, or an EC/ACPI `_BCM`) and this approach does not apply — stop and investigate that.

## Step 2 — make macOS offer the slider (NVRMFB change)

AppleBacklightDisplay is only created for a connection whose `connectFlags` reports
`kIOConnectionBuiltIn` **and** whose `getAppleSense()` returns an LCD. In NVRMFB's display
enumeration (both the boot path and the hotplug path), where it does
`r->internal = si.internal;`, additionally mark the *fake* display internal — only when its EDID is
the forced one, so a real monitor isn't mislabelled:

```cpp
r->internal = si.internal ||
    (dd->edid.bufferSize == sizeof kForcedEdid &&
     memcmp(dd->edid.buffer, kForcedEdid, sizeof kForcedEdid) == 0);
```

## Step 3 — give the backlight handler a target (NVRMFB change)

NVRMFB's `publishBacklight()` calls NVRM's `"NVRMBacklight"` platform function and bails when NVRM
has no backlight. Add a fallback: if that call fails, find the bridge kext by service matching and
hand the handler to it instead.

```cpp
if (r != kIOReturnSuccess) {
    OSDictionary *m = IOService::serviceMatching("PrimeBridge");   // <- your bridge's class
    OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr;
    if (m) m->release();
    IOService *pb = it ? OSDynamicCast(IOService, it->getNextObject()) : nullptr;
    if (pb) { pb->retain(); rm = pb; level = 80; r = kIOReturnSuccess; }
    if (it) it->release();
}
```

(`NVRMBacklightHandler::withRM(rm, level)` then sets `fRM = pb`, keeps `fFn = "NVRMBacklight"`,
and `getAppleSense()` starts returning `kGenericLCD` once `fBacklight` is non-NULL.)

If the fallback logs "no NVKMS backlight" and you never see the handler created, the bridge service
may not be published yet when `publishBacklight()` runs (it runs before `super::start()`); defer
the handler creation until after the bridge service appears.

## Step 4 — answer the brightness request in the bridge kext

The handler drives brightness with
`rm->callPlatformFunction("NVRMBacklight", false, &percent, (void*)1, NULL, NULL)` (and reads with
`param2 == NULL`). Override `callPlatformFunction` in the bridge kext:

```cpp
IOReturn Bridge::callPlatformFunction(const OSSymbol *fn, bool wait,
                                      void *p1, void *p2, void *p3, void *p4) {
    if (fn && fn->isEqualTo("NVRMBacklight")) {
        if (!p2) { if (p1) *(UInt32 *)p1 = 80; return kIOReturnSuccess; }   // read default %
        setPanelBrightness(p1 ? *(UInt32 *)p1 : 80);                       // write
        return kIOReturnSuccess;
    }
    return IOService::callPlatformFunction(fn, wait, p1, p2, p3, p4);
}
```

`setPanelBrightness(pct)` should map BAR5 lazily if needed, then do the same read-modify-write as
Step 1 (duty = `pct * 65535 / 100`, keep the other bits).

## Step 5 — verify

```bash
log show --last boot --predicate 'eventMessage CONTAINS "backlight"' --style compact | grep NVRM-fb
```
- Expect `no NVKMS backlight -- using PrimeBridge for the panel backlight` and then
  `backlight: brightness parameters on …`.
- Move the slider / press the brightness keys; the panel should dim, and the bridge's log should show
  `brightness: panel N% (PWRSEQ0_BL_PWM_CNTL …)`.

## Adaptation and failure notes

- The register offset depends on the **DCN version** — always take it from that version's
  `dcn_*_offset.h`/`_sh_mask.h`, and use the same segment base the bridge kext already uses.
- Some panels clamp minimum brightness; map 0% to a small non-zero duty if the panel blanks.
- If Step 1 did not dim the panel, the backlight is not the DCN PWM register: investigate eDP AUX
  DPCD `0x722` (DDC/AUX over the iGPU's eDP link) or an EC/ACPI `_BCM` method. Those are different
  pieces of work and may not be reachable from macOS at all.
- Keep the one-shot test boot-arg; it's the fastest way to re-prove the hardware path after a
  NullMoth update. Remove it once the slider works.

---

End of prompt.
