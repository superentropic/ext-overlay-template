# SDK 4888: silent-aim implementation status

**True silent aim is not implemented in this external application.**

The requested behavior is to redirect attacks to a selected bone while leaving
the cursor and camera untouched, respecting actual visibility and weapon reach.
The existing implementation wrote `AController::ControlRotation` when the mouse
button was pressed and restored it on release. That changes the view and is not
synchronized with attack generation. Those writes have been removed, and the UI
now reports the unavailable backend. Legacy `projectileRedirect` preferences are
still readable and saved for configuration compatibility; they do not activate
attack redirection.

## Supplied SDK evidence

Inspected dump:
`/home/enki/Downloads/ext-research-tool-main/dumps/sdk-complete-4888`

Paths below are relative to
`external-linux-DungeonCrawler.exe/CppSDK/SDK/`.

| SDK source | Finding |
| --- | --- |
| `Engine_classes.hpp` | `AController::ControlRotation` is the controller rotation at `0x0320`, not a separate attack-only direction. |
| `Engine_classes.hpp` | `GetBoneName`, `GetSocketLocation`, and `LineOfSightTo` are in-engine functions, not externally callable memory fields. |
| `DungeonCrawler_classes.hpp` | Melee abilities use `UGA_MeleeAttackBase` / `UGA_PlayerCharMeleeAttackBase` and `ADCGATA_LineCollision` target data. |
| `DungeonCrawler_classes.hpp` | Ranged abilities use `UGA_RangedAttackBase` and `ADCGATA_AimTraceToSocket`; this is a distinct attack path. |
| `DungeonCrawler_functions.cpp` | Generated `ReceivedTargetData` and `FireProjectile` wrappers dispatch through `UObject::ProcessEvent`; they do not contain the game's attack implementation. |
| `DungeonCrawler_functions.cpp` | `UDCAbilitySystemBlueprintLibrary::GetAimViewPoint` and `IsTargetLocationSightBlocked` likewise require engine execution. |
| `GameplayAbilities_classes.hpp` | Target actors may produce target data on the server (`ShouldProduceTargetDataOnServer`). A client-side memory write does not establish server acceptance. |
| `../../../sdk-validation.json` (dump root) | The validation scope explicitly covers generated declarations/layout assertions, with no linking or target calls. |

## Missing runtime capabilities

The current `Process` interface and kernel transport copy memory. They do not
provide a synchronized attack callback or an engine collision query. The SDK
does not establish a persistent, universal attack-direction field that can be
reliably changed externally for melee, projectiles, and spells.

Screen projection does not establish line of sight. A fixed distance limit does
not establish weapon reach. The previous estimated head/chest positions also do
not prove that a particular named bone was selected.

Consequently, external-only, any-hit redirection cannot be claimed from these
headers and the existing transport. A verified attack-time integration and real
collision/range validation remain prerequisites. Successful compilation,
driver smoke tests, and actor-scanning logs do not validate silent aim.
