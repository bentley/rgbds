mac: MACRO
	if (\1) < 10
		println "small \1"
	elif (\1) > 100
		println "large \1"
	elif (\1) / 0 == 42 ; only evaluated if the "large" condition was taken
		println "division by zero!?"
	elif syntax! error?
		println "X_X"
	else
		println "unreachable"
	endc
ENDM

	mac 2 + 2
	mac STRLEN("abcdef")
	mac 101
