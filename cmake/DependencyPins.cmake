# Dependency revisions are source-build candidates, not vendored game or script-extender binaries.
set(SYNTH_COMMONLIBF4_REPOSITORY "https://github.com/libxse/CommonLibF4.git")
set(SYNTH_COMMONLIBF4_REVISION "6266ecc9014b473fc6b6efd04abac324477c63cd")
# Header-only consumer API for the optional in-game menu host. Only
# resources/F4SEMenuFramework.h and resources/DIK.h are used; SYNTH links
# nothing from it and resolves the host module at runtime.
set(SYNTH_MENU_FRAMEWORK_REPOSITORY "https://github.com/DCCStudios/F4SEMenuFramework.git")
set(SYNTH_MENU_FRAMEWORK_REVISION "b031040dcb9b89d5b0accf8a4e4c99733f4dd63a")
set(SYNTH_COMMONLIBF4VR_REPOSITORY "https://github.com/ArthurHub/CommonLibF4VR.git")
set(SYNTH_COMMONLIBF4VR_REVISION "1c7b4fc860261eabad9f044e336965c26abe8ee6")
set(SYNTH_VR_ADDRESS_LIBRARY_REPOSITORY "https://github.com/alandtse/fallout_vr_address_library.git")
set(SYNTH_VR_ADDRESS_LIBRARY_REVISION "8f8d65e5941c17c88576e7b1185f26ea337f068f")
set(SYNTH_VR_ADDRESS_LIBRARY_RELEASE "v1.13.1")
set(SYNTH_F4SEVR_VERSION "0.6.21")
set(SYNTH_FALLOUT4VR_RUNTIME "1.2.72")
