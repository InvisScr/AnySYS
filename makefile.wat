# SPDX-License-Identifier: GPL-2.0-or-later

EXE = AnySYS.exe
CC = wcl
CFLAGS = -bt=dos -ms -os -s -w4 -zq

# wcl leaves the object in the current directory, not next to the source,
# and names it for the host: .o on Linux, .obj on DOS/Windows.
!ifdef __UNIX__
OBJ = anysys.o
RM = rm -f
!else
OBJ = anysys.obj
RM = del
!endif

all: $(EXE)

$(EXE): src/anysys.c
    $(CC) $(CFLAGS) -fe=$(EXE) src/anysys.c

clean: .SYMBOLIC
    -$(RM) $(EXE)
    -$(RM) $(OBJ)
