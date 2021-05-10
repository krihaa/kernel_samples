# bootblock.s

# .equ symbol, expression. 
# This directive sets the value of symbol to expression
	.equ	BOOT_SEGMENT,0x07c0
	.equ	DISPLAY_SEGMENT,0xb800

.text                  # Code segment
.globl _start	       # The entry point must be global
.code16                # Real mode

#
# The first instruction to execute in a program is called the entry
# point. The linker expects to find the entry point in the "symbol" _start
# (with underscore).
#
_start:

#
# Do not add any instructions before or in os_size.
#
	
	jmp	over
os_size:
	# area reserved for createimage to write the OS size
	.word	0
	.word	0
over:
  mov $0x07c0,%ax             # sett ax,ds til starten av bootblock
  mov %ax,%ds
  mov $startup_msg,%si        # skriv ut en melding
  call start_write
  jmp load                    # hopp til innlesning fra disk

start_write:                  # skriver ut ved å bruke bios funksjon int 10 0x0e
  mov $0x0e,%ah               # bios utskriftfunsjon
  mov $0x00,%bh               # side tall
  mov $0x02,%bl               # farge
write:
  lodsb                       # forkortelse for å flytte %si inn i %al og øke si
  cmp $0,%al                  # sjekke om 0, .asciz setter automatisk 0 byte på slutten.
  je finish_write             # hvis vi traff 0, hopp til ferdig med å skrive
  int $0x10                   # skriv ut det i %al
  jmp write                   # vi fortsetter loopen til vi treffer 0 byte

finish_write:
  xor %si,%si                 # 0 stiller de pekerne vi brukte
  xor %al,%al                 # sikkert ikke nødvendig men greit å ikke ha de hengene rundt
  ret                         # returnerer til kaller

load_fail:                    # hvis vi ikke leste inn os riktig
  mov $load_err,%si           # skriv ut en feilmelding og avslutt (gå til forever)
  call start_write
  jmp forever

load:
  mov $load_msg,%si
  call start_write
  xor %ax,%ax
  mov $0x02,%ah               # bios funksjon 2
  mov os_size,%al             # sektorer å lese
  mov $0x00,%ch               # spor number
  mov $0x02,%cl               # sektorer å begynne på, bootblock er på 1, os på 2+
  mov $0x00,%dh               # hodet
  mov $0x80,%dl               # disk type: usb minne
  mov $0x0800,%bx             # adresse å laste inn på ES:BX
  mov %bx,%es                 # flytter 0x0800 inn i ES
  xor %bx,%bx                 # offset i BX 0. bli da 0x0800:0000
  int $0x13                   # kaller int 13 funksjon 2
  jc load_fail                # hvis carry er satt gikk noe feil, hopp til load_fail

kernel_setup:
  mov $load_cml,%si
  call start_write
  mov $0x8000,%ax            # Setter stackpointer til
  mov %ax,%sp                # øverst ledig minne uten å skrive over extended-bios/vram
  sub $4096,%ax
  mov %ax,%ss                # Setter stacksegmentet til 4k under stackpointer

  xor %ax,%ax                 # reset ax,ds
  mov %ax,%ds
  ljmp $0x0800,$0x0000        # hopp til os/kernel start
forever:
	jmp	forever                 # Loop forever

# utskrift meldinger
startup_msg:
  .asciz "Bootloader started\n\r"
load_msg:
  .asciz "Loading kernel...\n\r"
load_cml:
  .asciz "Load complete\n\rStarting kernel\n\r"
load_err:
  .asciz "Error reading kernel from disc\n\r"


  .fill 510-(.-_start), 1, 0 # fyller inn de ledige plassene av 510 bytes totalt med null
  .word 0xaa55               # setter de siste 2 bytene til 55AA, som forteller bios at denne er bootable
