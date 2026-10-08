#include "menu.h"
#include "drawing.h"
#include "core/process.h"
#include <imgui.h>
#include <cstdio>
#include <string>

static constexpr const char* VERSION = "v1.0.0";

void Menu::Render() {
    if (!m_visible) return;

    float S = Drawing::GetDpiScale();

    ImGui::SetNextWindowSize(ImVec2(400 * S, 540 * S), ImGuiCond_FirstUseEver);

    // Custom title with version
    char title[64];
    snprintf(title, sizeof(title), "DAD Research Tool %s###DRT", VERSION);

    if (ImGui::Begin(title, &m_visible, ImGuiWindowFlags_NoCollapse)) {

        // ---- Status bar ----
        {
            ImU32 statusColor = m_attached ? IM_COL32(80, 220, 80, 255) : IM_COL32(220, 80, 80, 255);
            ImGui::PushStyleColor(ImGuiCol_Text, statusColor);
            ImGui::Text(m_attached ? "ATTACHED" : "NOT ATTACHED");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::Text("FPS: %.0f", m_fps);
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::Text("Entities: %d", m_entityCount);
        }
        ImGui::Separator();
        ImGui::Spacing();

        // ---- Mode ----
        ImGui::Checkbox("External", &m_config.externalMode);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Run standalone without the DLL.\n"
                              "Disables proxy scanning, the proxy status tag and silent aim.\n"
                              "ESP, loot and minimap keep working.");
        ImGui::Spacing();

        // ---- ESP Filters ----
        if (ImGui::CollapsingHeader("ESP Filters", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(4.0f * S);

            ImGui::Checkbox("Players", &m_config.filter.showPlayers);
            ImGui::SameLine(140 * S);
            ImGui::Checkbox("NPCs", &m_config.filter.showNPCs);

            ImGui::Checkbox("Loot", &m_config.filter.showLoot);
            ImGui::SameLine(140 * S);
            ImGui::Checkbox("Portals", &m_config.filter.showPortals);

            ImGui::Checkbox("Corpses", &m_config.filter.showCorpses);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Show or hide dead player and monster bodies");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::SliderFloat("Player Range (m)", &m_config.filter.maxPlayerDistance, 50.0f, 1000.0f, "%.0f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Player ESP range filter.\n"
                    "NOTE: The game server only replicates player transforms within ~25m\n"
                    "(network relevancy). Beyond that, Ghost markers show last-known positions.");

            ImGui::SliderFloat("Other Range (m)", &m_config.filter.maxDistance, 10.0f, 300.0f, "%.0f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Range for NPCs, loot, chests, portals");

            ImGui::Spacing();

            // Rarity filter starts at Uncommon — Junk/Poor/Common are never useful
            const char* rarityNames[] = {"Uncommon", "Rare", "Epic", "Legendary", "Unique"};
            const int RARITY_OFFSET = static_cast<int>(ItemRarity::UNCOMMON); // First selectable rarity
            int rarityIdx = static_cast<int>(m_config.filter.minLootRarity) - RARITY_OFFSET;
            if (rarityIdx < 0) rarityIdx = 0;
            if (rarityIdx >= IM_ARRAYSIZE(rarityNames)) rarityIdx = IM_ARRAYSIZE(rarityNames) - 1;
            if (ImGui::Combo("Min Loot Rarity", &rarityIdx, rarityNames, IM_ARRAYSIZE(rarityNames))) {
                m_config.filter.minLootRarity = static_cast<ItemRarity>(rarityIdx + RARITY_OFFSET);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Filters ground loot, chest contents, AND dead body equipment");

            const char* gradeNames[] = {"All", "Common+", "Elite+", "Nightmare"};
            ImGui::Combo("Min NPC Grade", &m_config.filter.minMonsterGrade, gradeNames, IM_ARRAYSIZE(gradeNames));

            ImGui::Unindent(4.0f * S);
        }

        ImGui::Spacing();

        // ---- Visual Settings ----
        if (ImGui::CollapsingHeader("Visuals")) {
            ImGui::Indent(4.0f * S);

            const char* boxStyles[] = {"2D Box", "Filled Box", "Corner Box"};
            ImGui::Combo("Box Style", &m_config.visuals.boxStyle, boxStyles, IM_ARRAYSIZE(boxStyles));

            ImGui::Checkbox("Health Bars", &m_config.filter.showHealthBars);
            ImGui::SameLine(140 * S);
            ImGui::Checkbox("Damage / Healing", &m_config.filter.showDamageNumbers);

            ImGui::Checkbox("Distance", &m_config.filter.showDistance);

            ImGui::Checkbox("Snap Lines", &m_config.filter.showSnapLines);

            ImGui::Checkbox("Ghost Players", &m_config.filter.showGhostPlayers);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Show last-known-position markers for players\n"
                    "whose transforms went dormant (beyond ~25m).\n"
                    "Ghosts expire after ~60 seconds.");

            ImGui::Spacing();

            // Font size slider — allows user to adjust ESP text/UI size
            if (ImGui::SliderFloat("Font Size", &m_config.visuals.fontSize, 10.0f, 30.0f, "%.0f")) {
                m_fontSizeChanged = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Requires restart to take effect");
            if (m_fontSizeChanged) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "(restart)");
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Text("Minimap");
            ImGui::SliderFloat("Size", &m_config.visuals.radarRadius, 80.0f, 300.0f, "%.0f px");
            ImGui::Checkbox("Rotate with Player", &m_config.visuals.minimapRotating);
            ImGui::TextDisabled("Range: 100 m");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::ColorEdit4("Player Color", m_config.visuals.playerColor,
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
            ImGui::SameLine();
            ImGui::Text("Enemy");
            ImGui::ColorEdit4("Team Color", m_config.visuals.teamColor,
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
            ImGui::SameLine();
            ImGui::Text("Teammate");
            ImGui::SameLine(200 * S);
            ImGui::ColorEdit4("NPC Color", m_config.visuals.npcColor,
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
            ImGui::SameLine();
            ImGui::Text("NPC");

            ImGui::Unindent(4.0f * S);
        }

        ImGui::Spacing();

        // ---- Silent Aim / SilentHelper Settings (hidden in External mode) ----
        if (!m_config.externalMode && ImGui::CollapsingHeader("Silent Aim (SilentHelper)")) {
            ImGui::Indent(4.0f * S);

            // Detailed Proxy Live Diagnostics Card
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.11f, 0.90f));
            ImGui::BeginChild("ProxyStatusCard", ImVec2(0, 125 * S), true);

            if (m_proxyTel.isConnected) {
                ImGui::TextColored(ImVec4(0.25f, 1.0f, 0.35f, 1.0f), "● PROXY CONNECTED & ACTIVE");
                ImGui::SameLine(220 * S);
                ImGui::TextDisabled("Heartbeat: #%u", m_proxyTel.heartbeat);

                ImGui::TextDisabled("Payload Base:");
                ImGui::SameLine(95 * S);
                ImGui::Text("0x%llX (SHM @ 0x%llX)", (unsigned long long)m_proxyTel.moduleBase, (unsigned long long)m_proxyTel.shmAddr);

                ImGui::TextDisabled("VMT Hooks:");
                ImGui::SameLine(95 * S);
                if (m_proxyTel.hooksInstalled) {
                    ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "● Active (Ctrl=#%u, View=#%u, Redir=#%u)",
                        m_proxyTel.hookCallCountController, m_proxyTel.hookCallCountLocalPlayer, m_proxyTel.redirectCount);
                    // Show how many times GetViewPoint applied a redirect in silent-camera mode.
                    if (m_proxyTel.viewPointRestoreCount > 0) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), " [VPRedir=%u]",
                            m_proxyTel.viewPointRestoreCount);
                    }
                } else if (m_proxyTel.lastHookStatus > 1) {
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.2f, 1.0f), "✗ Hook failed (error code %u)", m_proxyTel.lastHookStatus);
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "○ Waiting for Game Instance...");
                }

                ImGui::TextDisabled("Engine Pointers:");
                ImGui::SameLine(95 * S);
                ImGui::Text("Ctrl: 0x%llX | Player: 0x%llX",
                    (unsigned long long)m_proxyTel.playerControllerAddr,
                    (unsigned long long)m_proxyTel.localPlayerAddr);

                ImGui::TextDisabled("Redirection:");
                ImGui::SameLine(95 * S);
                if (m_proxyTel.isRedirecting) {
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f), "[ACTIVE REDIRECTING] Pitch=%.1f° Yaw=%.1f°",
                        m_proxyTel.redirectPitch, m_proxyTel.redirectYaw);
                } else if (m_proxyTel.hasTarget) {
                    ImGui::TextColored(ImVec4(0.3f, 0.85f, 1.0f, 1.0f), "Target Locked (%.1fm away)", m_proxyTel.targetDistance);
                } else {
                    ImGui::TextDisabled("Standby (No target in FOV cone)");
                }
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.2f, 1.0f), "○ PROXY NOT DETECTED (Offline)");
                if (m_proxyTel.manualBaseFailed) {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Manual base 0x%llX failed: no valid payload found.",
                        (unsigned long long)m_proxyTel.manualBase);
                } else if (m_proxyTel.manualBase != 0) {
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "Searching at manual base 0x%llX...",
                        (unsigned long long)m_proxyTel.manualBase);
                } else {
                    ImGui::TextWrapped("Map DLL into game process or place version.dll in game folder.\n"
                                       "Scanning memory automatically...");
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // ---- Manually Mapped Module Discovery Input Field ----
            ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "Mapped Module Base Address:");

            if (!m_manualBaseBufferInit) {
                if (m_config.projectileRedirect.customBaseAddress != 0) {
                    snprintf(m_manualBaseBuffer, sizeof(m_manualBaseBuffer), "0x%llX",
                             (unsigned long long)m_config.projectileRedirect.customBaseAddress);
                } else {
                    snprintf(m_manualBaseBuffer, sizeof(m_manualBaseBuffer), "0x0");
                }
                m_manualBaseBufferInit = true;
            }

            ImGui::SetNextItemWidth(170 * S);
            bool enterPressed = ImGui::InputText("##MappedBaseInput", m_manualBaseBuffer, sizeof(m_manualBaseBuffer),
                                                 ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Hexadecimal base address of the manually mapped DLL in target process.\n"
                                  "Example: 0x7FFA12340000 or 7FFA12340000.\n"
                                  "Press Enter or click Apply to discover the module.\n"
                                  "Set to 0x0 or click Auto-Scan to restore automatic memory discovery.");
            }

            ImGui::SameLine();
            bool applyClicked = ImGui::Button("Apply");
            ImGui::SameLine();
            bool autoScanClicked = ImGui::Button("Auto-Scan");

            if (autoScanClicked) {
                snprintf(m_manualBaseBuffer, sizeof(m_manualBaseBuffer), "0x0");
                m_config.projectileRedirect.customBaseAddress = 0;
                m_configStore.Save();
                if (m_process) {
                    m_process->SetManualProxyBase(0);
                }
            } else if (applyClicked || enterPressed) {
                uintptr_t newBase = 0;
                try {
                    std::string str(m_manualBaseBuffer);
                    while (!str.empty() && (str.front() == ' ' || str.front() == '\t')) str.erase(str.begin());
                    while (!str.empty() && (str.back() == ' ' || str.back() == '\t')) str.pop_back();
                    if (!str.empty()) {
                        newBase = static_cast<uintptr_t>(std::stoull(str, nullptr, 16));
                    }
                } catch (...) {
                    newBase = 0;
                }
                m_config.projectileRedirect.customBaseAddress = newBase;
                if (newBase != 0) {
                    snprintf(m_manualBaseBuffer, sizeof(m_manualBaseBuffer), "0x%llX", (unsigned long long)newBase);
                } else {
                    snprintf(m_manualBaseBuffer, sizeof(m_manualBaseBuffer), "0x0");
                }
                m_configStore.Save();
                if (m_process) {
                    m_process->SetManualProxyBase(newBase);
                }
            }

            // Live status indicator for manual base
            if (m_proxyTel.manualBase != 0) {
                if (m_proxyTel.isConnected) {
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "● Module verified at 0x%llX (Active)",
                                       (unsigned long long)m_proxyTel.manualBase);
                } else if (m_proxyTel.manualBaseFailed) {
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.2f, 1.0f), "✗ Module NOT found at 0x%llX (Scan failed)",
                                       (unsigned long long)m_proxyTel.manualBase);
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f), "⏳ Locating module at 0x%llX...",
                                       (unsigned long long)m_proxyTel.manualBase);
                }
            } else {
                if (m_proxyTel.isConnected) {
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "● Auto-discovered module at 0x%llX",
                                       (unsigned long long)m_proxyTel.moduleBase);
                } else {
                    ImGui::TextDisabled("Mode: Memory auto-discovery (no manual base specified)");
                }
            }

            ImGui::Spacing();
            ImGui::Checkbox("Enable Silent Redirection", &m_config.projectileRedirect.enabled);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Silently redirects outgoing weapon swings, arrows, and spells\n"
                                  "to the selected target bone without moving the viewport camera.");

            if (m_config.projectileRedirect.enabled) {
                ImGui::Spacing();

                // Target bone selection
                const char* bones[] = { "Head", "Neck", "Chest", "Nearest to Crosshair" };
                int currentBone = m_config.projectileRedirect.bone;
                if (currentBone < 0 || currentBone > 3) currentBone = 0;
                if (ImGui::Combo("Target Bone", &currentBone, bones, IM_ARRAYSIZE(bones))) {
                    m_config.projectileRedirect.bone = currentBone;
                }

                // Activation key selection
                const char* keys[] = { "Always Active", "Right Mouse Button (RMB)", "Left Mouse Button (LMB)", "Shift Key", "Alt Key", "V Key" };
                int currentKeyIdx = 0;
                if (m_config.projectileRedirect.key == 0) currentKeyIdx = 0;
                else if (m_config.projectileRedirect.key == 0x02) currentKeyIdx = 1; // VK_RBUTTON
                else if (m_config.projectileRedirect.key == 0x01) currentKeyIdx = 2; // VK_LBUTTON
                else if (m_config.projectileRedirect.key == 0x10) currentKeyIdx = 3; // VK_SHIFT
                else if (m_config.projectileRedirect.key == 0x12) currentKeyIdx = 4; // VK_MENU
                else if (m_config.projectileRedirect.key == 'V')  currentKeyIdx = 5;

                if (ImGui::Combo("Activation Key", &currentKeyIdx, keys, IM_ARRAYSIZE(keys))) {
                    switch (currentKeyIdx) {
                        case 0: m_config.projectileRedirect.key = 0; break;
                        case 1: m_config.projectileRedirect.key = 0x02; break; // VK_RBUTTON
                        case 2: m_config.projectileRedirect.key = 0x01; break; // VK_LBUTTON
                        case 3: m_config.projectileRedirect.key = 0x10; break; // VK_SHIFT
                        case 4: m_config.projectileRedirect.key = 0x12; break; // VK_MENU
                        case 5: m_config.projectileRedirect.key = 'V'; break;
                    }
                    m_config.projectileRedirect.requireKey = (currentKeyIdx != 0);
                }

                ImGui::SliderFloat("FOV Limit (deg)", &m_config.projectileRedirect.fov, 1.0f, 180.0f, "%.1f°");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Maximum angular distance from crosshair to acquire target.");

                ImGui::SliderFloat("Max Range (m)", &m_config.projectileRedirect.maxTargetDistance, 2.0f, 100.0f, "%.1f m");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Maximum distance to target in meters.");

                ImGui::Checkbox("Target Players", &m_config.projectileRedirect.targetPlayers);
                ImGui::SameLine(180 * S);
                ImGui::Checkbox("Target Monsters", &m_config.projectileRedirect.targetNPCs);

                ImGui::Spacing();
                ImGui::SliderFloat("Aim Z Offset (UU)", &m_config.projectileRedirect.targetZOffset, -300.0f, 100.0f, "%.0f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Vertical offset applied to the target bone position.\n"
                        "Negative values aim lower (e.g. -50 = neck, -100 = chest).\n"
                        "Default: 0 (no offset).");

                ImGui::Checkbox("Show FOV Circle", &m_config.projectileRedirect.showFOVCircle);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Draw a ring showing the redirect FOV boundary.\n"
                        "Grey=idle  Cyan=locked  Green=firing");

                ImGui::Spacing();
                ImGui::Checkbox("Silent Camera (recommended)", &m_config.projectileRedirect.silentCamera);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "ON (default): redirect fires but camera and ESP never move.\n"
                        "OFF (debug): camera visibly snaps toward target — confirms redirect is working.");
            }

            ImGui::Unindent(4.0f * S);
        }

        if (!m_config.externalMode) ImGui::Spacing();

        // ---- Performance / Stealth ----
        if (ImGui::CollapsingHeader("Performance / Stealth")) {
            ImGui::Indent(4.0f * S);

            ImGui::Checkbox("Low Footprint Mode", &m_config.performance.lowFootprintMode);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Reduces memory reads/writes, increases caching, lowers scan frequency.\n"
                    "Recommended for avoiding detection. May slightly reduce ESP responsiveness.");

            if (m_config.performance.lowFootprintMode) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "STEALTH MODE ACTIVE");
                ImGui::TextDisabled("Memory access optimized for minimal footprint");

                ImGui::Spacing();
                ImGui::SliderInt("Scan Interval (ms)", &m_config.performance.scanIntervalMs, 500, 5000, "%d");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Actor scan frequency. Higher = less memory access, slower entity updates.");

                ImGui::SliderInt("Position Update (ms)", &m_config.performance.positionUpdateIntervalMs, 8, 100, "%d");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How often positions are refreshed. Higher = less reads, more extrapolation.");

                ImGui::Checkbox("Batch Memory Reads", &m_config.performance.batchMemoryReads);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Combine multiple reads into single syscalls where possible.");

                ImGui::Checkbox("Disable Expensive ESP", &m_config.performance.disableExpensiveESP);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Disables bone ESP, health bars, snaplines, damage numbers.\n"
                        "Keeps only basic boxes and names for maximum performance.");

                ImGui::SliderInt("Max Entities/Frame", &m_config.performance.maxEntitiesProcessed, 32, 512, "%d");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Hard limit on entities processed per render frame.");

                ImGui::Checkbox("Reduce Scan Thread Priority", &m_config.performance.reduceScanThreadPriority);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Lowers background scan thread priority to reduce CPU contention.");
            } else {
                ImGui::Spacing();
                ImGui::TextDisabled("Enable Low Footprint Mode to configure stealth settings");
            }

            ImGui::Unindent(4.0f * S);
        }

        ImGui::Spacing();

        // ---- Performance (Legacy) ----
        if (ImGui::CollapsingHeader("Performance (Legacy)")) {
            ImGui::Indent(4.0f * S);
            ImGui::TextDisabled("Render");
            ImGui::SameLine(100 * S);
            ImGui::Text("%d FPS cap (no vsync)", m_config.updateRateFps);
            ImGui::TextDisabled("Scan");
            ImGui::SameLine(100 * S);
            ImGui::Text("~2 Hz background thread");
            ImGui::TextDisabled("Position");
            ImGui::SameLine(100 * S);
            ImGui::Text("Per-frame with extrapolation");
            ImGui::TextDisabled("Scale");
            ImGui::SameLine(100 * S);
            ImGui::Text("%.1fx (auto)", S);
            ImGui::Unindent(4.0f * S);
        }

        ImGui::Spacing();

        // ---- Debug ----
        if (ImGui::CollapsingHeader("Debug")) {
            ImGui::Indent(4.0f * S);
            ImGui::TextDisabled("GWorld");
            ImGui::SameLine(120 * S);
            ImGui::Text("0x%llX", (unsigned long long)m_gworldAddr);
            ImGui::TextDisabled("GNames");
            ImGui::SameLine(120 * S);
            ImGui::Text("0x%llX", (unsigned long long)m_gnamesAddr);
            ImGui::TextDisabled("Entities");
            ImGui::SameLine(120 * S);
            ImGui::Text("%d tracked", m_entityCount);

            if (!m_config.externalMode) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "Proxy & SilentHelper Telemetry");
                ImGui::Spacing();

                ImGui::TextDisabled("Proxy Status:");
                ImGui::SameLine(120 * S);
                if (m_proxyTel.isConnected) {
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "CONNECTED (Payload Base: 0x%llX)", (unsigned long long)m_proxyTel.moduleBase);
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.2f, 1.0f), "OFFLINE (Awaiting injection/mapping)");
                }

                ImGui::TextDisabled("Configured Base:");
                ImGui::SameLine(120 * S);
                if (m_proxyTel.manualBase != 0) {
                    ImGui::Text("0x%llX (%s)", (unsigned long long)m_proxyTel.manualBase,
                                m_proxyTel.isConnected ? "Verified" : (m_proxyTel.manualBaseFailed ? "Failed" : "Searching"));
                } else {
                    ImGui::TextDisabled("Auto-scan (0x0)");
                }

                ImGui::TextDisabled("Remote SHM Addr:");
                ImGui::SameLine(120 * S);
                ImGui::Text("0x%llX", (unsigned long long)m_proxyTel.shmAddr);

                ImGui::TextDisabled("Heartbeat Counter:");
                ImGui::SameLine(120 * S);
                ImGui::Text("%u ticks", m_proxyTel.heartbeat);

                ImGui::TextDisabled("Target Bone Feed:");
                ImGui::SameLine(120 * S);
                if (m_proxyTel.hasTarget) {
                    ImGui::Text("(%.1f, %.1f, %.1f) [%.1fm]", m_proxyTel.targetX, m_proxyTel.targetY, m_proxyTel.targetZ, m_proxyTel.targetDistance);
                } else {
                    ImGui::TextDisabled("None");
                }

                ImGui::TextDisabled("Redirection State:");
                ImGui::SameLine(120 * S);
                if (m_proxyTel.isRedirecting) {
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f), "ACTIVE (Pitch=%.1f°, Yaw=%.1f°)", m_proxyTel.redirectPitch, m_proxyTel.redirectYaw);
                } else {
                    ImGui::TextDisabled("IDLE");
                }
            }

            ImGui::Unindent(4.0f * S);
        }

        // ---- Footer ----
        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Save Settings")) {
            m_configStore.Save();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Writes config/settings.json now");
        ImGui::TextDisabled("INSERT - Toggle Menu  |  END - Exit");
    }
    ImGui::End();
}
