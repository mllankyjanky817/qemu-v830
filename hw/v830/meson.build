v832_ss = ss.source_set()
v832_ss.add(when: 'CONFIG_V832_BOARD', if_true: files('v832_board.c',
									 'v832_test_board.c',
									 'v832_soc.c',
									 'v832_bcu.c',
									 'v832_peripherals.c',
									 'v832_dma.c'))
hw_arch += {'v830': v832_ss}
