// SPDX-License-Identifier: BSD-2-Clause-Patent
// No-op definitions of the probe hooks the patched XNU references while tb128fu_defconfig leaves their component out.
// Each hook runs only on its boot argument, which board.toml does not pass; the kernel link needs the symbol anyway.

// offline-component-load-probes.patch, bsd_init: called on offline-legacy-register=1 (NativeLegacyDisplay). Weak: with
// CONFIG_XNU_NATIVE_LEGACY_DISPLAY the display archive's LegacyDisplayProbe.cpp defines the real probe.
void native_legacy_display_probe(void);
__attribute__((weak)) void native_legacy_display_probe(void)
{
}

// boot_progress_poll is a weak reference of NativeUfs/NativeVirtioBlock; CONFIG_XNU_BOOT_PROGRESS links the real
// definition from build/boot-progress, and an undefined weak symbol stays a null call when the page is off.
