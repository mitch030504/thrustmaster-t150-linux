# Release testing

The Linux control panel is developed against a T150RS in PS3/PC mode
(USB `044f:b677`). Record the kernel release, firmware version, pedal set,
and test date for physical runs. Record whether results are from a guest,
real wheel, or capture replay.

## Build and install

1. Run `make test`. This checks packet fixtures and settings math, profile
   persistence, force-test duration, and offline package inspection.
2. For each kernel header tree, run
   `make -f dkms_make.mak KDIR=/lib/modules/RELEASE/build all` and check
   `modinfo -F vermagic build/hid-t150.ko`. Cover GCC and Clang kernels.
   With KVM and an installed CachyOS kernel that builds USB Raw Gadget
   and Dummy HCD as modules, run `python tests/check_usb_guest.py RELEASE`
   to test probe, the short firmware reply, pedal mode, all nine slots,
   and the USB start/stop order in an isolated guest.
3. On a test machine with matching headers, run `sudo ./install.sh`
   or install both Arch packages. Confirm that `t150-control --diagnose`
   shows `hid-t150`, firmware, and supported effects. Use
   `udevadm info /dev/input/eventN` to confirm the joystick and uaccess
   tags on the wheel's event node.
4. Upgrade and uninstall using the same path. Check that saved profiles
   and local defaults survive, and that generic joystick input returns.
   On the Arch path check `dkms status` after installing and removing the
   package. Check the Fedora RPM separately on Fedora.

## Connected wheel and Proton

1. Let the wheel finish calibrating. Keep clear of the rim when
   starting a force test. Open Inputs in the panel and move the wheel
   through its travel. Verify center, stops at 270° and 1080°, both
   directions of the D-pad, all buttons, and each attached pedal.
2. Compare separate and combined pedal mode in `evtest`. In combined
   mode verify center at rest, opposite ends for accelerator and
   brake, and the remaining pedal active if attached. Restore separate
   mode after the check. Test shorter rotation ranges on hardware
   before exposing them.
3. Start with Sine at low strength. If the movement is too small to
   see, center the wheel, keep the rim clear, and choose **Play 50%
   sine for 1 second**. On a T150RS with firmware 35 and CachyOS
   7.4.0-rc3-1, the direct evdev probe measured 18 steering reversals
   and a span of 443 counts, with zero movement at idle. The panel's
   dedicated button also moved the same wheel during the hardware test.
   Check both directions of constant force and the other advertised
   waveforms separately. The standard test offers 5%, 10%, 25%, and
   50% strength. Spring and Damper run for two seconds so you can
   feel the resistance while turning the rim gently. Verify that Stop, closing the panel, and
   switching focus stop an active effect.
4. Exercise upload, play, stop, update, and erase in all nine slots.
   Stop while an update is queued and check that it stays stopped.
   Check removal, unplug/replug, and suspend/resume for stale effects.
   Use wheel-specific USB captures to compare packets when a test fails.
5. Save and reapply a profile, apply a shipped preset, and check the
   same standard attributes as a normal user in Oversteer. Verify an
   account outside the settings group cannot change wheel settings.
6. Add the official Live for Speed demo to Steam as a non-Steam game
   and select Proton. Disable Steam Input for that title. In wheel
   settings bind steering and pedals, start at low gain, and verify
   force feedback while driving and release of force when exiting.

## Release gates

The preview requires physical Linux acceptance through the procedure
above. Square, triangle, ramp, friction, and inertia need proven native
encoding or validated bounded software rendering before advertisement.
Validate native torque regulation and stop behavior against the wheel
before enabling more effects.

For native firmware, obtain an official updater USB trace with a verified
image, an independent spare T150, and a working Windows recovery station.
Identify the flashing and recovery interfaces from evidence. Exercise the
transaction against recorded and simulated USB responses, including a
wrong device, interrupted transfer, and mismatched firmware. Verify
installed firmware and inputs after a clean update and after recovering
from an interrupted update on the spare wheel. The currently shipped
package inspection never opens the USB device.
