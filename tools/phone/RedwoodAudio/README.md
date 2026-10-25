# RedwoodAudio.driver

The HAL plug-in (AudioServerPlugIn) that gives iOS 16's audio server (mediaserverd) its speaker
on taimen. It presents the devices the iPhone X's VirtualAudio expects: "Speaker", "Hawking",
"Actuator" and "Codec". It writes the mixed 48 kHz 16-bit stereo frames to `/dev/redwood-audio`,
and TaimenAudio (xnu) feeds them to the ADSP (docs/taimen-audio-plan.md, stages 9 and 10).

The code comes from HackiOS user space (<https://github.com/oosexgang/hackios-xnu-userland>, MIT
License, Copyright (c) 2026 OOSEX GANG; see LICENSE), as ios-topaz carries it in
`userland/audio`. The one taimen change is the manufacturer string.

tools/phone/build.sh builds it on the phone-tools CI as an ad hoc signed MH_BUNDLE.
`tools/taimen/mkrootfix.py --hal-plugin` installs it.
