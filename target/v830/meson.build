gen = [
  decodetree.process('insn16.decode', extra_args: [ '--decode', 'decode_insn16', '--insnwidth', '16' ]),
  decodetree.process('insn32.decode', extra_args: [ '--decode', 'decode_insn32', '--insnwidth', '32' ]),
]

v830_ss = ss.source_set()
v830_ss.add(gen)
v830_ss.add(files(
  'translate.c',
  'helper.c',
  'cpu.c',
  'gdbstub.c',
))

target_arch += {'v830': v830_ss}
