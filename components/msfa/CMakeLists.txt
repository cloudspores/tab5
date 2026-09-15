# msfa: the six-operator FM synthesis core from Google's "music synthesizer for android"
# (Apache License 2.0, see LICENSE). Vendored unchanged apart from a few public accessors in
# synth_unit.h noted with "tab5:". Fixed-point, portable C++; NEON paths are not used on RISC-V.
idf_component_register(
    SRCS "dx7note.cc" "env.cc" "exp2.cc" "fir.cc" "fm_core.cc" "fm_op_kernel.cc" "freqlut.cc"
         "lfo.cc" "log2.cc" "patch.cc" "pitchenv.cc" "resofilter.cc" "ringbuffer.cc" "sin.cc" "synth_unit.cc"
    INCLUDE_DIRS "."
)
# Third-party code: keep its warnings out of our warnings-as-errors build.
target_compile_options(${COMPONENT_LIB} PRIVATE -w -O2)
