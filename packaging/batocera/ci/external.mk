# pkg-emulator-info.mk is batocera's own, copied in by the workflow, so the
# package's $(eval $(emulator-info-package)) runs what it runs in batocera.
include $(BR2_EXTERNAL_BATOCERA_PATH)/pkg-emulator-info.mk
include $(BR2_EXTERNAL_BATOCERA_PATH)/package/libretro-m2hle/libretro-m2hle.mk
