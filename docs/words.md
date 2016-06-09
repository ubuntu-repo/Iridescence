### Stack effect

The **stack effect** notion rules are as follows:

* A stack effect is specified inside parenthesis **"("** and **")"**.


Notation | Meaning
-------- | -------
N        | Number
A        | Cell address
Ba       | Byte address
M        | Multiplier
Q        | Quotient
D        | Divisor
B        | Block number
X        | Non specified 1 cell of memory
R        | Remainder
C        | Cylinder or character
F        | Flag
ZF       | Zero flag
OF       | Overflow flag
CF       | Carry flag
SF       | Sign flag


### colorForth Words

Word     | Stack effect | Meaning               | Dictionary
-------- | ------------ | -------               | ----------
!        | (a)          | Store word at address | Forth
