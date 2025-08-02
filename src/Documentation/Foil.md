# Different states:
## Idle: 
* always check Ax-Bx conditions
* none of the sides is currently debouncing
* no need to test point-guard, piste, Lame
* ok to check yellow light conditions
* ok to check weapon swithch conditions
* even in this state, limit the amount of additional checks per call
## Debouncing:
* at least one side Ax-Bx condition holds
* No other checks until at least one is fully debounced
* If debounced also check validity
* exit as soon as none of the sides are debouncing
* As soon as one side is debounced, reduce the debounce requirements for the other side
## Debounced
* At least one has reached the needed time
* Check Surface: Valid, Guard, Piste, 
* Question: if Guard or Piste, restart debounce or just continue until either White or Colored or not pressed down anymore?
This is a nasty state, because it requires many checks and therefor many extra phases, whic may impact the timing of the other side.

## Locking:
* One side has resulted in a signal (White or Colored)
* only monitor the other side, until the machine is locked

## Locked:
* Don't do anything

# Consequences:
## Re-order Phases & evaluate only after full scan for the specific state
### Idle: 3 checks
1. A1-B1
2. A2-B2
3. if(no start debounce conditions) -> measure one of the following (using a counter)
    * Orange Left
    * Orange Right
    * Long Al-Cl
    * Long Ar-Cr
    * Parry

In this way, we will always have <b>3</b> checks in this state

## Debouncing: 2 checks
* Check Al-Bl
* Check Ar-Br
* Skip all other phases, until one of the debounced conditions becomes true

This requires only 2 checks, and is therefor the fastest possible.

## Debounced: No need for a special state. Just perform these checks as soon as one side reaches debounced state
* For the side that reached the debouncing time: 3 checks
    * Guard
    * piste
    * lamé
* For the other side if not debounced yet: 1 check, else the same 3 checks

So a variable amount of checks: 4-6


Assuming a single check takes about 11 µs
Total time = n times 22µs Debouncing + 6*11µs validity check

Worst case spread: ~2*(Sampling time) ~ 44µs


Issues:
if the hit is on guard, or piste -> if we stay in debounced mode, we will have every time 3 extra checks, making the debouncing of the other side much less accurate
-> Let's just restart debouncing