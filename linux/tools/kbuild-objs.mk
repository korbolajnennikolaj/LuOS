# SPDX-License-Identifier: MIT
# Copyright (c) 2026 korbolajnennikolaj

# Evaluates a Kbuild directory Makefile with a given CONFIG set and prints, as KEY=VALUE lines,
# the object list, directory-wide flags and per-object flags.
# Evaluates a driver Kbuild with a CONFIG set; used by lkgen.py
.DEFAULT_GOAL := print
src := $(DRVDIR)
obj := $(DRVDIR)
# NAME turns an option on (y); NAME=VALUE gives it a value (integers)
$(foreach c,$(CFG),$(eval CONFIG_$(word 1,$(subst =, ,$(c))):=$(or $(word 2,$(subst =, ,$(c))),y)))
CC_FLAGS_FPU := -msse -msse2 -mhard-float
CC_FLAGS_NO_FPU := -mno-sse -mno-mmx -mno-sse2 -mno-3dnow -mno-avx
ifneq ($(wildcard $(DRVDIR)/Makefile),)
include $(DRVDIR)/Makefile
else
include $(DRVDIR)/Kbuild
endif
OBJS := $(sort $(foreach o,$($(MOD)-y) $($(MOD)-objs),$(o)))
print:
	@echo "OBJS=$(OBJS)"
	@echo "CCFLAGS=$(ccflags-y) $(subdir-ccflags-y) $(EXTRA_CFLAGS)"
	@$(foreach o,$(OBJS),$(if $(CFLAGS_$(o)),echo "CF:$(o)=$(CFLAGS_$(o))";)) true
	@$(foreach o,$(OBJS),$(if $(CFLAGS_REMOVE_$(o)),echo "CR:$(o)=$(CFLAGS_REMOVE_$(o))";)) true
