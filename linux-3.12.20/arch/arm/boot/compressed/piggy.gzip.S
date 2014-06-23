	.section .piggydata,#alloc
	.globl	input_data
input_data:
	.incbin	"arch/arm/boot/compressed/piggy.gzip"
	.globl	input_data_end
input_data_end:

/*!!C-----------------------------------------------------------------
 * .incbin 은 그 위치에 file 을 포함한다는 의미.
 -------------------------------------------------------------------*/
