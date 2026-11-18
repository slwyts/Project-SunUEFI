include debian/rules

# The Debian package revision must not alter the sealed kernel's build number.
revision := $(KBUILD_BUILD_VERSION)

.PHONY: debian-metadata
debian-metadata:
	sed -i '$$s/  .*$$/  $(shell date -R -d @$(SOURCE_DATE_EPOCH))/' debian/changelog
	awk -v package='linux-image-$(KERNELRELEASE)' \
	    -v depends="$$SUNUEFI_DEBIAN_IMAGE_DEPENDS" \
	    'BEGIN { RS=""; FS="\n"; ORS="\n\n" } \
	     $$1 == "Package: " package { \
	       found = 1; \
	       if (!index($$0, depends) && \
	           !sub(/\nDepends:[[:blank:]]*/, "\nDepends: " depends ", ", $$0)) \
	         $$0 = $$0 "\nDepends: " depends; \
	     } \
	     { print } \
	     END { if (!found || !length(depends)) exit 1 }' debian/control > debian/control.tmp
	mv debian/control.tmp debian/control

build-arch: debian-metadata
