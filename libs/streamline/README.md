# NVIDIA Streamline headers

Headers from NVIDIA's Streamline SDK
(<https://github.com/NVIDIA-RTX/Streamline>, release `v2.14.1`,
`streamline-sdk-v2.14.1.zip`, folder `include/`), unchanged: `sl.h` and
what it includes (`sl_struct.h`, `sl_consts.h`, `sl_version.h`,
`sl_result.h`, `sl_appidentity.h`, `sl_device_wrappers.h`,
`sl_core_api.h`, `sl_core_types.h`), `sl_dlss.h`, `sl_dlss_d.h`,
`sl_matrix_helpers.h`, `sl_security.h`, and `sl_helpers.h` with the
headers it includes (`sl_reflex.h`, `sl_pcl.h`, `sl_nis.h`,
`sl_dlss_g.h`). The other headers are not included; `sl_nvperf.h` in
particular is under NVIDIA's Nsight Perf SDK license, not MIT. No
Streamline or NVIDIA binaries are in this repository (PLAN §4 and §5):
players copy the DLLs from the release zip themselves
(`docs/hexenlicht/DLSS.md`).

License: MIT, see `LICENSE.txt` (the SDK's `license.txt`; its Nsight Perf
note covers `sl_nvperf.h` and `sl.nvperf.dll`, which are not here).

`engine/hexenlicht/vk_streamline.cpp` includes them. It loads
`sl.interposer.dll` at runtime (`LoadLibrary`, `GetProcAddress`) after
checking its signature with `sl_security.h`; nothing is linked.
