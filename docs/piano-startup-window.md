# Scoped product startup choice window

`PianoBootPolicyStartupWindow(3000)` is a separate APP-only API invoked once by
the resident product Core after owner/late-provider initialization and before
the first existing `PianoBootPolicyRun`. It does not change Run's dispatch or
default auto-SimpleInit behavior, execute an image, reset the tablet or stop any
shared service. Root's real USB service continues to receive APP pump slices.

During this window the existing F12 callback requests Setup. Esc gets a second,
temporary key-notify token on each current SimpleTextInEx provider and requests
SimpleInit only while `mStartupWindow` is true. Window exit clears that flag
before retiring the exact Esc tokens. The F12 tokens remain installed. After
the window, Esc retains the child's original back/cancel behavior; old or
removed providers are not dereferenced.

The actual TimerLib counter/properties determine elapsed time and direction.
Every slice pumps the service with a1000us budget, rechecks the actual pending
action, and stalls1000us while the window remains open. Pending Setup,
SimpleInit, Shell or parent-return ends the wait for normal later dispatch.
There is no fake key, secondary timer event, nested StartImage or SMMU probe.
Zero/invalid counter properties fail before enabling the window. Reversed
counter movement fails. A3100-slice ceiling returns EFI_TIMEOUT if the counter
freezes, instead of creating an unbounded startup wait. API duration is1..3000ms
and an already-used window is refused.

Notification warnings, a token returned with failure, success without a token,
or uncertain removal preserve the resident policy and refuse later dispatch.
Known register error with no token preserves the existing F12 registration and
reports the error. Successful removal uses the freshly verified current
interface and saved unregister method. Uninstalled/replaced providers own their
former callback lists; the policy never calls their inaccessible old object.
The existing EBS fence disables the window on CPU state only and blocks all
subsequent BS/keyboard/USB calls. The parent keeps retained code resident.

```
python3 -m unittest discover -s tests/unit -p test_boot_policy.py -v
```

The actual BootPolicy+FV loader C runs64 unchanged Run regression scenarios and
18 additional window scenarios under ASan/UBSan, with real Mu UEFI types and
strict ARM64 syntax checking. Window tests cover measured expiration, F12/Esc,
pending/real USB parent-return, callback scope outside the window, no image or
service-stop calls, count-down/frozen/reversed counters, APP/TPL enforcement,
failed registration/removal, partial ownership, unavailable old provider pages
and EBS with inaccessible gBS. Those fixtures validate source behavior, not
physical pogo/USB keyboard support. F12/Esc hardware acceptance remains separate.
