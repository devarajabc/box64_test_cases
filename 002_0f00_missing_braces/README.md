# 002_0f00_missing_braces

## Bug Description

Missing braces in `src/emu/x64run0f.c` around the `else` block for opcode `0x0F 0x00` (SLDT/STR/VERR/VERW instructions).

## Location

File: `src/emu/x64run0f.c`
Lines: 89-101

## The Bug

```c
        } else
            nextop = F8;              // <-- Only this line is in the else!
            switch((nextop>>3)&7) {   // <-- This ALWAYS executes!
                case 0:
                    ...
                default:
                    return 0;
            }
```

Due to C's syntax, only `nextop = F8;` is part of the `else` branch. The `switch` statement always executes regardless of `rex.is32bits`.

## Impact

- **32-bit mode (`rex.is32bits=true`)**: First switch handles STR/VERR/VERW correctly, but then the second switch also runs and returns 0 (unhandled opcode) for anything except SLDT.

- **64-bit mode (`rex.is32bits=false`)**: Second switch only handles SLDT (case 0). STR, VERR, VERW all hit `default: return 0;`

## Affected Instructions

| Instruction | Opcode | ModRM /reg | Affected? |
|-------------|--------|------------|-----------|
| SLDT | 0F 00 | /0 | No (handled in both switches) |
| STR | 0F 00 | /1 | **Yes** |
| VERR | 0F 00 | /4 | **Yes** |
| VERW | 0F 00 | /5 | **Yes** |

## Fix

Add braces around the else block:

```c
        } else {
            nextop = F8;
            switch((nextop>>3)&7) {
                case 0:                 /* SLDT Ew */
                    GETEW(0);
                    if(MODREG)
                        ED->q[0] = 0;
                    else
                        EW->word[0] = 0;
                    break;
                default:
                    return 0;
            }
        }
```

## Test Behavior

- **Without fix**: STR, VERR, VERW tests will fail with SIGILL or return failure
- **With fix**: All tests should pass

## Running

```bash
# Build
make

# Run on x86_64 (baseline - should all pass)
./002_0f00_missing_braces

# Run under Box64 (will show the bug)
BOX64_DYNAREC=0 box64 ./002_0f00_missing_braces
```

Note: `BOX64_DYNAREC=0` forces interpreter mode to test `x64run0f.c`. With dynarec enabled, the dynarec implementation may or may not have the same bug.
