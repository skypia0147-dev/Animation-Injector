#pragma once
namespace AnimationInjector::QteMenu {
void Register();
// Main thread, every frame: opens the menu when a QTE is on screen. It closes itself once nothing has been drawn for
// a moment, so no menu of ours sits above the HUD while idle (mods that read the top menu, Screen Archer Menu's
// hotkeys, see the HUD).
void Sync();
}
