; Patch by Conn
; apply on Super Mario World 2 - Yoshi's Island (US, v1.1) without header

; free ram
; $7e0160 - track number
; $7e0161 - msu busy (01: ready, 02: fade)
; $7e0162 - current volume for fading)

; header ; remove semicolon before header if you apply on headered rom




lorom


;------------------- hook area

org $00800D
JSL initial
NOP
NOP

org $008092
JSL startBank

org $0085b6
JSL stopTrackchange 
NOP

org $00c008  ; nmi hook
JSL $7efbd0


org $00C024
JSL $7efcb0  ;main msu routine
NOP

org $01C2c5
JSL pause
NOP

org $01Cfe5
JSL pause
NOP

org $01Db69
JSL stonedEnd

org $0FC598
JSL palFix
NOP
NOP



;touch
org $03F680
JSL stonedStart
;eat
org $039ED7
JSL stonedStart










;org $00c3f0  ;irq hook
;JSL $7ef7d0

org $0dd36d
jsl bowserfix



;-------------------- free space
;----------------------nmi hook
org $00Fbd0 ;nmi ram
LDA $7e0161 ;check busy flag
AND #$00FF
BEQ $03
JSR $fc00
LDA #$0000
TCD
RTL



org $00Fc00
LDA #$0000
TAX
PHP
SEP #$30
LDA $7e0161
cmp #$10
BEQ fade
LDA $7e0161
dec
cmp #$00
BEQ endCheck
STA $7e0161
CMP #$01
BNE endCheck
LDA #$F0
STA $002140
STA $7e004d
STA $7e0163
STA $7e0165
endCheck:
LDA $7e0165
cmp #$F0
BEQ $02
PLP
RTS
lda $002000
AND #$40
BEQ $08
LDA #$01
STA $7e0161
PLP
RTS
PHX
LDA $7e0160
TAX
LDA $7efe50,x ;load loop value
STA $002007  ; also wenn ein theme endlos looped dann mach hier eine 03 auf 2007
PLX
LDA #$00
STA $7e0161
STA $7e0165
LDA #$FF
STA $002006
STA $7e0162  ; volume control
PLP
RTS


fade:
LDA $7e0162
cmp #$00
beq mute
dec
dec
dec
STA $7e0162
STA $002006
cmp #$05
bcc mute
PLP
RTS
mute:
LDA #$00
STA $002006
STA $7e0161
STA $7e0162
PLP
RTS
;----------------------------------------------------------------------
;----------------------------------------------------------------------

org $01f000

initial:
STZ $4200
STZ $4016 
LDA #$00
STA $7e0160
STA $7e0161
STA $7e0162
STA $7e0163
STA $7e0164
RTL

bowserfix:
lda $7e0160
cmp #$1c
beq $04
lda #$01
STA $4d
rtl

startBank:
SEP #$10
LDA #$10
STA $0150 ; restore the music bank at intro
LDA #$01
RTL

stonedStart:
PHA
PHP
SEP #$20
LDA $2002
CMP #$53
BNE endStoneStart
LDA #$24           ; stoned flower garden
cmp $7e0160
BEQ endStoneStart
STZ $2006
STA $7e0160  ; store theme number
STA $2004
STZ $2005
LDA #$04
STA $7e0161  ; store msu busy flag
endStoneStart:
PLP
PLA
JSL $0085DE
RTL

stonedEnd:
PHA
PHP
SEP #$20
LDA $2002
CMP #$53
BNE endStoneend
LDA $0150       ; flower Garden
clc 
adc #$10
STZ $2006
STA $7e0160  ; store theme number
STA $2004
STZ $2005
LDA #$04
STA $7e0161  ; store msu busy flag
endStoneend:
PLP
PLA
JSL $0085De
RTL

pause:
AND #$01
STA $0B12
PHA
LDA $2002
CMP #$53
BEQ $02
PLA
RTL
PLA
CMP #$01
BNE $04
STZ $2007
RTL
CMP #$00
BNE $05
LDA #$03
STA $2007
RTL

stopTrackchange:
LDA #$ff
STA $2140
LDA $2002
CMP #$53
BNE $03
STZ $2007
RTL

palFix:
LDA #$02
STA $004d
RTL

;--------------------------------------
;---------------------main msu routine
org $00Fcb0
PHA
BEQ $05
BRL newTrack
BRA returnNewTrack
LDA $7e0164
BEQ returnNewTrack
LDA #$02
STA $7e0161
LDA #$00
STA $7e0164
returnNewTrack:
PLA
STA $4F
STA $2140
RTL




; -------------------main theme mapping
newTrack:
PHA
LDA $2002
CMP #$53
BEQ $04
PLA
BRL returnNewTrack

PLA
CMP #$F0   ; check mute
BNE nomute
LDA $7e0163
cmp #$F0
BEQ $03
STZ $2007
LDA #$00
STA $7e0163
LDA #$F0
BRA endTrack
nomute:    
CMP #$F1   ; check fade
BNE $08
LDA #$10
STA $7e0161
BRA endTrack
BRL mapThemes
endTrack:
BRL returnNewTrack



mapThemes:
; check if on map
PHA
lda $0150
cmp #$12
BNE $06
PLA
LDA #$22       ; we are on the map
brl endMapThemes
PLA

; all identical
cmp #$04
BNE $03
BRL endMapThemes
cmp #$06
BNE $03
BRL endMapThemes
cmp #$07
BNE $03
BRL endMapThemes
cmp #$08
BNE $03
BRL endMapThemes

; different in theme 02
cmp #$02
BNE $03
brl track2

; different in theme 03
cmp #$03
BNE $03
brl track3

; different in theme 05
cmp #$05
BNE $03
brl track5

; different in theme 09
cmp #$09
BNE $03
brl track9
identicalTrack1:

; different in theme 09
cmp #$09
BNE $03
brl track9

; different in theme 0a
cmp #$0a
BNE $03
brl tracka


; different in theme 0c
cmp #$0c
BNE $05
lda #$19
brl endMapThemes

cmp #$01
BNE $03
brl track1


;security branch
LDA #$00
brl endTrack

;--------------------track differences mapping
track2:
LDA $0150 ; load music bank
cmp #$10
BNE $05
LDA #$0A
brl endMapThemes
CMP #$11
BNE $05
LDA #$0B
brl endMapThemes
cmp #$13
BNE $05
LDA #$25      ; ending theme 2
brl endMapThemes
LDA #$02      ; powerful infant else
brl endMapThemes

track3:
LDA $0150 ; load music bank
cmp #$10
BNE $05
LDA #$20 ;yoshi Island (title screen) at night, a different theme can be given here
brl endMapThemes
LDA #$03
brl endMapThemes

track5:
LDA $0150 ; load music bank
cmp #$07
BEQ bigBoss
cmp #$08
BEQ bigBoss
cmp #$09
BEQ bigBoss
LDA #$05
brl endMapThemes
bigBoss:
LDA #$0C
BRA endMapThemes

track9:
LDA $0150 ; load music bank
cmp #$03
BEQ kamek
cmp #$07
BEQ kamek
cmp #$0D
BEQ kamek
cmp #$0C
BEQ luigi
LDA #$01
brl track1
kamek:
LDA #$18
brl endMapThemes
luigi:
LDA #$0D
brl endMapThemes

tracka:
LDA $0150 ; load music bank
cmp #$0C
bne $05
LDA #$0E  ; baby bowser victory
brl endMapThemes
CMP #$03
BEQ mini
CMP #$05
BEQ mini
CMP #$0d
BEQ mini
LDA #$19  ; play big boss theme
brl endMapThemes
mini:
LDA #$0F  ; play mini boss theme
brl endMapThemes


track1:
CLC
lda $0150
adc #$10  ; add new index
CMP #$15
BNE $02
LDA #$18 ; redundant Kamek
CMP #$1B 
BNE $02
LDA #$02 ; redundant power star
CMP #$1D
BNE $02
LDA #$13 ; redundant room before boss
CMP #$1E
BNE $02
LDA #$12 ; redundant castle theme
CMP #$1F
BNE $02 
LDA #$20 ; redundant intro
brl endMapThemes

;------------- end Mapping
endMapThemes:
STZ $2006
STA $7e0160  ; store theme number
STA $7e0164
STA $2004
STZ $2005

brl endTrack

; loop table 
org $00Fe50
;  0   1   2   3   4   5   6   7   8   9   A   B   C   D   E   F
db $03,$03,$03,$01,$01,$01,$03,$01,$01,$03,$03,$03,$01,$03,$01,$03 ; themes 00-0f 
db $03,$03,$03,$03,$03,$03,$03,$03,$03,$03,$03,$03,$03,$03,$03,$03 ; themes 10-1f 
db $03,$03,$03,$01,$03,$01,$03,$03,$03,$03,$03,$03,$03,$03,$03,$03 ; themes 20-2f 




; Theme Map
; hex   dec  loop  tag
; 02    02   y     Powerful Infant https://www.youtube.com/watch?v=qyZcCGPPN9U
; 03    03   n     Game Start https://www.youtube.com/watch?v=rtF7NtBExec
; 04    04   n     Game Over https://www.youtube.com/watch?v=Fme9om_p89U 
; 05    05   n     Goal https://www.youtube.com/watch?v=R1lCiWOlFUA
; 06    06   y     Score https://www.youtube.com/watch?v=kLFeMQutTpU
; 07    07   n     Player Down https://www.youtube.com/watch?v=O3GHn42H5EM
; 08    08   n     Mario Kidnapped https://www.youtube.com/watch?v=LPlTbZEdriE
;
; 0A    10   y     Story Music Box https://www.youtube.com/watch?v=ipDsethN0mk
; 0B    11   y     Training Course https://www.youtube.com/watch?v=BwsU6CrDCNY
; 0C    12   n     Big Boss Clear https://www.youtube.com/watch?v=KAhUU4pdwM8
; 0D    13   y     Luigi rescued https://www.youtube.com/watch?v=Y2kLK6q7Wa4
; 0E    14   n     Baby Bowser Clear: https://www.youtube.com/watch?v=4ELQPkqxiX8
; 0F    15   y     Miniboss: https://www.youtube.com/watch?v=JSBTSW_B81Y
; 10    16   y     Flower Garden: https://www.youtube.com/watch?v=kMmpRcld1FU
; 11    17   y     Overworld Theme: https://www.youtube.com/watch?v=9-jva3PUKZw
; 12    18   y     Castle&Fortress: https://www.youtube.com/watch?v=KBMonSXsUh4
; 13    19   y     Room before boss: https://www.youtube.com/watch?v=EFGYFysGhns
; 14    20   y     Underground: https://www.youtube.com/watch?v=-HOrFykW5ew
;
; 16    22   y     bonus game: https://www.youtube.com/watch?v=7zTCKNeWW4U
; 17    23   y     Room before boss: https://www.youtube.com/watch?v=EFGYFysGhns
; 18    24   y     Kamek's theme: https://www.youtube.com/watch?v=GHAWoTonyhA
; 19    25   y     Big Boss theme: https://www.youtube.com/watch?v=Q--ywLM84Ng
; 1A    26   y     Athletic Theme: https://www.youtube.com/watch?v=GlUeW7IOSFc
; 
; 1C    28   y     Bowser End: https://www.youtube.com/watch?v=vbFQh32xlh0
;
;
; 
; 20    32   y     Yoshi's Island https://www.youtube.com/watch?v=grVbhes4ceY! 
; 21    33   y     Yoshi Start Demo https://www.youtube.com/watch?v=X50QWCZfASw
; 22    34   y     Map: https://www.youtube.com/watch?v=9xZUISErsxg
; 23    35   n     ending theme: https://www.youtube.com/watch?v=ZAPaGu0fWDE
; 24    36   y     Stoned flower garden: https://www.youtube.com/watch?v=QXswn1BKnS8
; 25    37   n     ending theme2: https://www.youtube.com/watch?v=MGM6FiDti04
