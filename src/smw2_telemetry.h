/* SMW2 widescreen telemetry — per-frame state ring + screenshot sidecars.
 *
 * Purpose: correlate a player-reported glitch screenshot with the EXACT
 * guest + widescreen state at that instant. One CSV row per simulated frame
 * (ws_telemetry.csv beside the exe, wall-clock anchored) plus a JSON sidecar
 * written next to every hotkey screenshot (in the screenshots dir, beside each .bmp).
 *
 * Observer only: reads guest/policy state, never changes it.
 */
#ifndef SMW2_TELEMETRY_H_
#define SMW2_TELEMETRY_H_

/* Append one row for the frame just rasterized. Call once per frame from
 * the title's begin_sim_frame hook. */
void Smw2TelemetryFrame(unsigned frame);

/* Write <path>.json with the current state (host screenshot hook). */
void Smw2TelemetrySidecar(const char *path);

#endif /* SMW2_TELEMETRY_H_ */
