# Keep the generated upstream changelog stable across builds of one ABI.
include debian/rules

.PHONY: changelog-date
changelog-date:
	sed -i '$$s/  .*$$/  $(shell date -R -d @$(SOURCE_DATE_EPOCH))/' debian/changelog

build-arch: changelog-date
