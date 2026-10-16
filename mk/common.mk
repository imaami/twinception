override MK_DIR := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))
override SRCDIR := $(realpath $(MK_DIR)../src)/

override TGT = $(eval \
  override TGT:=$$(foreach b,$$(BIN),$$(if $$(strip $$(SRC_$$b)),$$b)))$(TGT)

override A_TGT  = $(eval override A_TGT:=$$(filter %.a,$$(TGT)))$(A_TGT)
override O_TGT  = $(eval override O_TGT:=$$(filter-out %.a %.so,$$(TGT)))$(O_TGT)
override SO_TGT = $(eval override SO_TGT:=$$(filter %.so,$$(TGT)))$(SO_TGT)

.PHONY: all $(TGT)             \
        compile_commands.json  \
        clean $(TGT:%=clean-%) \
        purge $(TGT:%=purge-%) \
        clean-compile_commands.json
all: | $(TGT) compile_commands.json

# Nothing below this comment is defined when tab-completing.

ifneq (.DEFAULT,$(MAKECMDGOALS))

# No `override` here, it clobbers command-line variables.
CFLAGS     :=
CPPFLAGS   := -Wall -Wextra -Wpedantic -DNDEBUG=1
CXXFLAGS   :=
LDLIBS     :=
LDFLAGS    :=
PKG_CONFIG := pkg-config

# Here `override` is necessary for the appending to work.
ifeq (,$(filter -flto=% -flto -fno-lto,$(CFLAGS)))
override CFLAGS += -flto=auto
endif
ifeq (,$(filter -std=%,$(CFLAGS)))
override CFLAGS += -std=gnu23
endif
ifeq (,$(filter -O -O%,$(CFLAGS)))
override CFLAGS += -O3
endif
ifeq (,$(filter -mcpu=% -march=% -mtune=%,$(CFLAGS)))
override CFLAGS += -march=native -mtune=native
endif

ifeq (,$(filter -flto=% -flto -fno-lto,$(CXXFLAGS)))
override CXXFLAGS += -flto=auto
endif
ifeq (,$(filter -std=%,$(CXXFLAGS)))
override CXXFLAGS += -std=gnu++17
endif
ifeq (,$(filter -O -O%,$(CXXFLAGS)))
override CXXFLAGS += -O3
endif
ifeq (,$(filter -mcpu=% -march=% -mtune=%,$(CXXFLAGS)))
override CXXFLAGS += -march=native -mtune=native
endif

# Plugin loader needs default symbol visibility to be hidden.
ifeq (,$(filter -fvisibility=%,$(CFLAGS)))
override CFLAGS += -fvisibility=hidden
endif
ifeq (,$(filter -fvisibility=%,$(CXXFLAGS)))
override CXXFLAGS += -fvisibility=hidden
endif

override _cc_var = $(shell $(wordlist 2,$(words $1),$1))
override __cc_var = $(call _cc_var,$(foreach x,$1, || { command -v "$x" >/dev/null && echo "$x"; } ))
override ___cc_var = $(eval override $1=$$(eval override $1:=$$$$(or $$$$(call __cc_var,$2),$3))$$($1))
override ____cc_var = $(call ___cc_var,$1,$2,$(if $(filter default,$(origin $1)),$(value $1)))
override _____cc_var = $(if $(filter default environment,$(origin $1)),$(call ____cc_var,$1,$2))
override compiler-var = $(call _____cc_var,$(strip $1),$(strip $2))

$(call compiler-var,CC,clang gcc)
$(call compiler-var,CXX,clang++ g++)

include $(MK_DIR)o.mk

override define __pkgcfg
$(eval
  ifneq (override,$$(origin __pkgcfg__$1__$2))
    override __pkgcfg__$1__$2 = $$(eval \
    override __pkgcfg__$1__$2 := $$(shell \
      $$(PKG_CONFIG) --$1 $2))$$(__pkgcfg__$1__$2)
  endif)
endef
override __pkg-config = $(call __pkgcfg,$1,$2)$(__pkgcfg__$1__$2)
override pkg-config = $(sort $(foreach x,$2,$(call __pkg-config,$(strip $1),$x)))
override __pkgcfg_inc_cflags = $(foreach x,$1,$(eval override \
 C$(if $(x:%.c=),XX)FLAGS_$x = $$(call pkg-config,cflags,$$(INC_$x))))

override define __tgt

override A_SRC_$1 := $$(filter %.a,$$(SRC_$1))
override O_SRC_$1 := $$(strip $$(SRC_$1:%.a=))
override A_OBJ_$1 := $$(A_SRC_$1:%=$$O%)
override O_OBJ_$1 := $$(O_SRC_$1:%=$$O%.o$$(if $(1:%.so=),,-fPIC))
override DEP_$1   := $$(O_SRC_$1:%=$$O%.d$$(if $(1:%.so=),,-fPIC))
override JSON_$1  := $$(O_OBJ_$1:=.json)

override DEP   += $$(DEP_$1)
override O_OBJ += $$(O_OBJ_$1)
override SRC   += $$(SRC_$1)

$1: | $$O$1

ifneq (,$(1:%.a=))
# Executable or shared library target
$$O$1: $$(A_OBJ_$1) $$(O_OBJ_$1) $$(EXT_$1)
override LDFLAGS_$1 += $$(call pkg-config,libs-only-L,$$(LIB_$1))
override LDLIBS_$1  += $$(call pkg-config,libs-only-l,$$(LIB_$1))
else
# Archive target
$$O$1: $$O$1($$(A_OBJ_$1) $$(O_OBJ_$1) $$(EXT_$1))
endif

clean-$1: | clean.o-$1 clean.json-$1
purge-$1: | clean-$1 clean.d-$1

# Delete .a dependencies only during purge
ifneq (,$$(A_SRC_$1))
.PHONY: | clean.a-$1
purge-$1: | clean.a-$1
endif
endef

override define targets
$(eval override DEP   :=)
$(eval override O_OBJ :=)
$(eval override SRC   :=)

$(foreach x,$1,$(eval $(call __tgt,$x)))

$(eval
override DEP   := $$(sort $$(DEP))
override O_OBJ := $$(sort $$(O_OBJ))
override SRC   := $$(sort $$(SRC))
override A_SRC := $$(filter %.a,$$(SRC))
override O_SRC := $$(strip $$(SRC:%.a=))
override A_OBJ := $$(A_SRC:%=$$O%)
override JSON  := $$(O_OBJ:=.json)
)

$(call __pkgcfg_inc_cflags,$(SRC))
endef

$(call set-build-dir,$(SRCDIR)../build)
$(call targets,$(TGT))

compile_commands.json: | $Ocompile_commands.json
$Ocompile_commands.json: $(JSON); jq -s '.' $^ > "$@"

purge: | clean clean.d clean-compile_commands.json
clean: | clean.o clean.json

clean:     ; $(RM) $(TGT:%=$O%)
clean.d:   ; $(RM) $(DEP)
clean.o:   ; $(RM) $(O_OBJ)
clean.json:; $(RM) $(JSON)

ifneq (,$(A_SRC))
.PHONY: | clean.a
purge: | clean.a
clean.a:; $(RM) $(A_OBJ)
endif

# Targets that need the output directory
$(TGT:%=$O%) $(A_OBJ) $(O_OBJ) $(JSON) $Ocompile_commands.json: | $O

ifneq (,$(O_TGT))
$(O_TGT:%=$O%):
	+$(strip $(if $(filter %.cpp,$(SRC_$(@F))),$(CXX),$(CC)) \
	               -o "$@" -MMD                \
	               $(if $(filter %.cpp,$(SRC_$(@F))),$(CXXFLAGS),$(CFLAGS)) \
	               -ffile-prefix-map=$(SRCDIR)='' \
	               $^ $(LDFLAGS) $(LDFLAGS_$(@F)) \
	               $(LDLIBS) $(LDLIBS_$(@F)))
endif

ifneq (,$(SO_TGT))
$(SO_TGT:%=$O%):
	+$(strip $(CC) -o "$@" -MMD $(CFLAGS)         \
	               -ffile-prefix-map=$(SRCDIR)='' \
	               -shared                        \
	               $^ $(LDFLAGS) $(LDFLAGS_$(@F)) \
	               $(LDLIBS) $(LDLIBS_$(@F)))
endif

ifneq (,$(A_TGT))
(%): % ;
$(A_TGT:%=$O%):
	$(AR) rcsU $@ $?
endif

$O%.c.o-fPIC: $(SRCDIR)%.c
	+$(strip $(CC) -o "$@" -fPIC                  \
	               $(CPPFLAGS) $(CPPFLAGS_$(<F))  \
	               $(CFLAGS) $(CFLAGS_$(<F))      \
	               -ffile-prefix-map="$(<D:/=)/=" \
	               -MF "$(@:o-fPIC=d-fPIC)"       \
	               -MMD -c "$<")

$O%.cpp.o-fPIC: $(SRCDIR)%.cpp
	+$(strip $(CXX) -o "$@" -fPIC                  \
	                $(CPPFLAGS) $(CPPFLAGS_$(<F))  \
	                $(CXXFLAGS) $(CXXFLAGS_$(<F))  \
	                -ffile-prefix-map="$(<D:/=)/=" \
	                -MF "$(@:o-fPIC=d-fPIC)"       \
	                -MMD -c "$<")

$O%.c.o: $(SRCDIR)%.c
	+$(strip $(CC) -o "$@" $(CPPFLAGS)            \
	               $(CPPFLAGS_$(<F)) $(CFLAGS)    \
	               $(CFLAGS_$(<F))                \
	               -ffile-prefix-map="$(<D:/=)/=" \
	               -MMD -c "$<")

$O%.cpp.o: $(SRCDIR)%.cpp
	+$(strip $(CXX) -o "$@" $(CPPFLAGS)            \
	                $(CPPFLAGS_$(<F)) $(CXXFLAGS)  \
	                $(CXXFLAGS_$(<F))              \
	                -ffile-prefix-map="$(<D:/=)/=" \
	                -MMD -c "$<")

$(TGT:%=clean-%) clean-compile_commands.json:
	$(RM) $O$(@:clean-%=%)

ifneq (,$(A_SRC))
$(TGT:%=clean.a-%):
	$(RM) $(A_OBJ_$(@:clean.a-%=%))
endif

$(TGT:%=clean.d-%):
	$(RM) $(DEP_$(@:clean.d-%=%))

$(TGT:%=clean.o-%):
	$(RM) $(O_OBJ_$(@:clean.o-%=%))

$(TGT:%=clean.json-%):
	$(RM) $(JSON_$(@:clean.json-%=%))

$O%.c.o.json: $(SRCDIR)%.c
	@printf '%s\0' $(CC) -o "$(@:.json=)"         \
	               $(CPPFLAGS) $(CPPFLAGS_$(<F))  \
	               $(CFLAGS) $(CFLAGS_$(<F))      \
	               -ffile-prefix-map="$(<D:/=)/=" \
	               -MMD -c "$<"                   \
	| jq -MRs --arg dir "$(O:/%/=/%)"             \
	          --arg src "$<"                      \
	          --arg out "$(@:.json=)"             \
	     "{ directory: \$$dir,                    \
	        arguments: (split(\"\u0000\")[:-1]),  \
	        file: \$$src, output: \$$out }" > "$@"

$O%.cpp.o.json: $(SRCDIR)%.cpp
	@printf '%s\0' $(CXX) -o "$(@:.json=)"        \
	               $(CPPFLAGS) $(CPPFLAGS_$(<F))  \
	               $(CXXFLAGS) $(CXXFLAGS_$(<F))  \
	               -ffile-prefix-map="$(<D:/=)/=" \
	               -MMD -c "$<"                   \
	| jq -MRs --arg dir "$(O:/%/=/%)"             \
	          --arg src "$<"                      \
	          --arg out "$(@:.json=)"             \
	     "{ directory: \$$dir,                    \
	        arguments: (split(\"\u0000\")[:-1]),  \
	        file: \$$src, output: \$$out }" > "$@"

$O%.c.o-fPIC.json: $(SRCDIR)%.c
	@printf '%s\0' $(CC) -o "$(@:.json=)" -fPIC   \
	               $(CPPFLAGS) $(CPPFLAGS_$(<F))  \
	               $(CFLAGS) $(CFLAGS_$(<F))      \
	               -ffile-prefix-map="$(<D:/=)/=" \
	               -MF "$(@:o-fPIC.json=d-fPIC)"  \
	               -MMD -c "$<"                   \
	| jq -MRs --arg dir "$(O:/%/=/%)"             \
	          --arg src "$<"                      \
	          --arg out "$(@:.json=)"             \
	     "{ directory: \$$dir,                    \
	        arguments: (split(\"\u0000\")[:-1]),  \
	        file: \$$src, output: \$$out }" > "$@"

$O%.cpp.o-fPIC.json: $(SRCDIR)%.cpp
	@printf '%s\0' $(CXX) -o "$(@:.json=)" -fPIC  \
	               $(CPPFLAGS) $(CPPFLAGS_$(<F))  \
	               $(CXXFLAGS) $(CXXFLAGS_$(<F))  \
	               -ffile-prefix-map="$(<D:/=)/=" \
	               -MF "$(@:o-fPIC.json=d-fPIC)"  \
	               -MMD -c "$<"                   \
	| jq -MRs --arg dir "$(O:/%/=/%)"             \
	          --arg src "$<"                      \
	          --arg out "$(@:.json=)"             \
	     "{ directory: \$$dir,                    \
	        arguments: (split(\"\u0000\")[:-1]),  \
	        file: \$$src, output: \$$out }" > "$@"

$O:; mkdir -p "$@"

-include $(DEP)
endif
