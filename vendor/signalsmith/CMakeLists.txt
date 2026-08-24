# Signalsmith Stretch and the signalsmith-linear FFT it sits on, from Cog
# ThirdParty/signalsmith (Signalsmith Audio, MIT). Header-only; the stretch
# header is the repository root single-header build, the linear headers are the
# repository root ones the include/ shims in that repository point back at.
#
# Vendored rather than a port because upstream tags trail what Cog ships against
# and the two libraries have to agree with each other version-for-version --
# taking both from Cog's tree is what keeps them the pair Cog's stretcher was
# tuned on. No backend macro is defined, so the portable plain-C++ FFT is what
# builds everywhere; SIGNALSMITH_USE_ACCELERATE and friends stay available to a
# platform that wants them later.
add_library(vendor-signalsmith INTERFACE)
# SYSTEM, so the headers compile under the consumer's warning flags without
# drowning the build in upstream's conversion warnings -- the same treatment
# XPCogCodec.cmake gives every decoder's vendored includes.
target_include_directories(vendor-signalsmith SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
