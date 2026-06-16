<?php
/**
 * analytics.php — AI-powered energy analytics via Google Gemini
 *
 * INFERENCE PIPELINE
 * ------------------
 * 1. Time-gate check: generation is only attempted during the 6 AM and 6 PM
 *    windows (Philippine Time). A 5.5-hour minimum gap between successive
 *    generations prevents double-firing within the same window.
 *
 * 2. Aggregation: daily and monthly stats are computed from raw sensor_readings
 *    rows (averages, peaks, energy delta, projected bill).
 *
 * 3. Gemini call: the aggregated stats are embedded into a structured prompt
 *    that constrains the model to return valid JSON with a fixed schema
 *    (energy rating A–F, numeric bill prediction, recommendation list, tip,
 *    summary). Philippine electricity context (₱10–₱12/kWh, 220 V/60 Hz) is
 *    included so the model calibrates its advice to local conditions.
 *
 * 4. Response parsing: the raw text is stripped of any markdown fences Gemini
 *    may add, then decoded. Invalid JSON causes the generation to be silently
 *    skipped — the previous recommendation is served instead.
 *
 * 5. TTS trigger: after storing the new recommendation, a fire-and-forget
 *    cURL POST to generate_tts.php converts the text to a BMO-personality
 *    WAV file without blocking the analytics response.
 *
 * Response JSON shape:
 *   { latest, history[6], generated: bool, next_at }
 */

header("Content-Type: application/json");
header("Access-Control-Allow-Origin: *");

require_once __DIR__ . '/config.php';

$env       = parse_ini_file(__DIR__ . '/.env');
$geminiKey = $env['GEMINI_API_KEY'] ?? '';

// All timestamps are stored and compared in Philippine Time (UTC+8).
$conn->query("SET time_zone = '+08:00'");

$now    = new DateTime('now', new DateTimeZone('Asia/Manila'));
$hour   = (int)$now->format('H');
$period = ($hour < 12) ? 'morning' : 'evening';

// Force generation with ?force=1 (for testing)
$force = isset($_GET['force']) && $_GET['force'] === '1';

// Time windows: 06:00–06:59 and 18:00–18:59
$isWindow = ($hour === 6 || $hour === 18) || $force;

// Check last generation
$lastRow = $conn->query(
    "SELECT * FROM ai_recommendations ORDER BY generated_at DESC LIMIT 1"
)->fetch_assoc();

// Generation is suppressed if fewer than 5.5 hours have elapsed since the last
// run — this prevents the 6 AM cron from firing twice if the server is hit
// multiple times within the same hour window.
$shouldGenerate = false;
if ($isWindow && $geminiKey) {
    if (!$lastRow) {
        $shouldGenerate = true;
    } else {
        $lastGen   = new DateTime($lastRow['generated_at'], new DateTimeZone('Asia/Manila'));
        $diffHours = ($now->getTimestamp() - $lastGen->getTimestamp()) / 3600;
        if ($diffHours >= 5.5 || $force) {
            $shouldGenerate = true;
        }
    }
}

if ($shouldGenerate) {
    $stats  = fetchStats($conn, $now);
    $aiData = callGemini($stats, $geminiKey, $now, $period);
    if ($aiData) {
        storeRecommendation($conn, $aiData, $stats, $period, $now);
        $lastRow = $conn->query(
            "SELECT * FROM ai_recommendations ORDER BY generated_at DESC LIMIT 1"
        )->fetch_assoc();

        // Fire-and-forget: trigger TTS audio generation in background.
        // Uses a 1-second timeout so analytics doesn't block waiting for Gemini TTS.
        $ch = curl_init('https://pmo.infinityfree.me/generate_tts.php');
        curl_setopt_array($ch, [
            CURLOPT_POST           => true,
            CURLOPT_POSTFIELDS     => '',
            CURLOPT_TIMEOUT_MS     => 1000,
            CURLOPT_RETURNTRANSFER => false,
            CURLOPT_SSL_VERIFYPEER => false,
        ]);
        @curl_exec($ch);
        curl_close($ch);
    }
}

// Return last 6 for history display
$history = [];
$res = $conn->query("SELECT * FROM ai_recommendations ORDER BY generated_at DESC LIMIT 6");
while ($row = $res->fetch_assoc()) { $history[] = $row; }

echo json_encode([
    'latest'    => $lastRow,
    'history'   => $history,
    'generated' => $shouldGenerate,
    'next_at'   => getNextGenerationTime($now),
]);
$conn->close();

// ── Helpers ───────────────────────────────────────────────────────

/**
 * Aggregate today's and this month's readings into a stats array.
 * Energy is derived as MAX(energy_kwh) − MIN(energy_kwh) for the period
 * because energy_kwh is a cumulative counter on the PZEM sensor — a simple
 * average would not give consumed kWh.
 */
function fetchStats(mysqli $conn, DateTime $now): array {
    $today      = $now->format('Y-m-d');
    $monthStart = $now->format('Y-m-01');

    $day = $conn->query("
        SELECT
            AVG(voltage)      avg_voltage,
            AVG(current_a)    avg_current,
            AVG(power_w)      avg_power,
            AVG(power_factor) avg_pf,
            AVG(frequency_hz) avg_freq,
            MAX(energy_kwh) - MIN(energy_kwh) today_energy,
            AVG(cost_per_hour) avg_cost_hour,
            COUNT(*)          readings
        FROM sensor_readings
        WHERE DATE(recorded_at) = '$today'
    ")->fetch_assoc();

    $month = $conn->query("
        SELECT
            MAX(energy_kwh) - MIN(energy_kwh) month_energy,
            AVG(cost_per_month)               avg_cost_month
        FROM sensor_readings
        WHERE recorded_at >= '$monthStart'
    ")->fetch_assoc();

    $peak = $conn->query("
        SELECT MAX(power_w) peak_power, MAX(current_a) peak_current
        FROM sensor_readings
        WHERE DATE(recorded_at) = '$today'
    ")->fetch_assoc();

    $daysInMonth = (int)$now->format('t');
    $dayOfMonth  = (int)$now->format('j');
    $daysLeft    = $daysInMonth - $dayOfMonth;

    $monthEnergy = (float)($month['month_energy'] ?? 0);
    $monthCost   = (float)($month['avg_cost_month'] ?? 0);

    // Linear bill projection: (cost so far / days elapsed) × days in month.
    $projBill    = $dayOfMonth > 0
        ? round(($monthCost / $dayOfMonth) * $daysInMonth, 2)
        : 0;

    return [
        'avg_voltage'    => round($day['avg_voltage']  ?? 0, 2),
        'avg_current'    => round($day['avg_current']  ?? 0, 3),
        'avg_power'      => round($day['avg_power']    ?? 0, 2),
        'avg_pf'         => round($day['avg_pf']       ?? 0, 3),
        'avg_freq'       => round($day['avg_freq']     ?? 0, 2),
        'peak_power'     => round($peak['peak_power']  ?? 0, 2),
        'peak_current'   => round($peak['peak_current']?? 0, 3),
        'today_energy'   => round($day['today_energy'] ?? 0, 4),
        'month_energy'   => round($monthEnergy, 4),
        'month_cost'     => round($monthCost, 2),
        'projected_bill' => $projBill,
        'days_elapsed'   => $dayOfMonth,
        'days_in_month'  => $daysInMonth,
        'days_left'      => $daysLeft,
        'readings_today' => (int)($day['readings'] ?? 0),
    ];
}

/**
 * Send the aggregated stats to Gemini and return parsed AI output.
 *
 * PROMPT ENGINEERING NOTES
 * ------------------------
 * - The system persona ("energy analytics AI for a Philippine household") sets
 *   domain context so the model applies local rates and voltage standards.
 * - Philippine electricity rate and typical consumption figures are injected
 *   so Gemini can calibrate its bill prediction and recommendations without
 *   hallucinating generic Western values.
 * - The output schema is hardcoded in the prompt ("Respond ONLY with valid JSON")
 *   to avoid free-text responses that would require additional parsing.
 * - Markdown fence stripping handles cases where the model wraps its JSON in
 *   ```json … ``` despite the explicit instruction not to.
 *
 * Returns null if the API call fails or the response is not valid JSON.
 */
function callGemini(array $stats, string $apiKey, DateTime $now, string $period): ?array {
    $datetime    = $now->format('D, d M Y H:i T');
    $periodLabel = ucfirst($period);

    $prompt = <<<PROMPT
You are an energy analytics AI for a Philippine household power monitoring system called PMO (Power Monitoring Operator).

Current date/time: {$datetime} (Philippine Time)
Analysis period: {$periodLabel}

Today's energy data:
- Average Voltage: {$stats['avg_voltage']} V  (PH nominal: 220 V)
- Average Current: {$stats['avg_current']} A
- Peak Current:    {$stats['peak_current']} A
- Average Active Power: {$stats['avg_power']} W
- Peak Active Power:    {$stats['peak_power']} W
- Average Power Factor: {$stats['avg_pf']}  (ideal ≥ 0.90)
- Average Frequency:    {$stats['avg_freq']} Hz  (PH standard: 60 Hz)
- Energy Consumed Today: {$stats['today_energy']} kWh
- Energy Consumed This Month: {$stats['month_energy']} kWh
- Estimated Month Cost So Far: ₱{$stats['month_cost']}
- Days Elapsed in Month: {$stats['days_elapsed']} of {$stats['days_in_month']} ({$stats['days_left']} days remaining)
- Rough Linear Projected Bill: ₱{$stats['projected_bill']}
- Sensor Readings Today: {$stats['readings_today']}

Context: Philippine electricity rate ≈ ₱10–₱12 per kWh (Meralco). Typical household monthly consumption 200–400 kWh.

Respond ONLY with valid JSON — no markdown, no code fences, no extra text:
{
  "energy_rating": "A/B/C/D/F letter grade",
  "rating_score": integer 0–100,
  "predicted_bill": number in PHP pesos (end-of-month prediction),
  "recommendations": [
    "actionable recommendation 1",
    "actionable recommendation 2",
    "actionable recommendation 3"
  ],
  "efficiency_tip": "one specific tip relevant to this time of day",
  "summary": "2-sentence plain-language summary of the current energy situation and monthly outlook"
}
PROMPT;

    $payload = json_encode([
        'contents' => [['parts' => [['text' => $prompt]]]]
    ]);

    $ch = curl_init(
        "https://generativelanguage.googleapis.com/v1beta/models/gemini-flash-latest:generateContent"
    );
    curl_setopt_array($ch, [
        CURLOPT_RETURNTRANSFER => true,
        CURLOPT_POST           => true,
        CURLOPT_POSTFIELDS     => $payload,
        CURLOPT_HTTPHEADER     => [
            'Content-Type: application/json',
            'X-goog-api-key: ' . $apiKey,
        ],
        CURLOPT_TIMEOUT        => 20,
    ]);
    $response = curl_exec($ch);
    curl_close($ch);

    if (!$response) return null;

    $decoded = json_decode($response, true);
    $text    = $decoded['candidates'][0]['content']['parts'][0]['text'] ?? '';

    // Strip markdown code fences if Gemini adds them despite the prompt instruction.
    $text = preg_replace('/^```(?:json)?\s*/i', '', trim($text));
    $text = preg_replace('/\s*```$/', '', trim($text));

    $aiData = json_decode(trim($text), true);
    if (!$aiData) return null;

    $aiData['raw_response'] = $text;
    return $aiData;
}

/**
 * Persist the parsed AI response and the input stats that produced it.
 * Storing both allows post-hoc auditing of whether the model's advice was
 * calibrated to accurate sensor data.
 */
function storeRecommendation(
    mysqli $conn, array $aiData, array $stats,
    string $period, DateTime $now
): void {
    $generatedAt    = $now->format('Y-m-d H:i:s');
    $energyRating   = $conn->real_escape_string($aiData['energy_rating']  ?? '?');
    $ratingScore    = intval($aiData['rating_score']   ?? 0);
    $predictedBill  = floatval($aiData['predicted_bill'] ?? 0);
    $recommendations= $conn->real_escape_string(json_encode($aiData['recommendations'] ?? []));
    $efficiencyTip  = $conn->real_escape_string($aiData['efficiency_tip'] ?? '');
    $summary        = $conn->real_escape_string($aiData['summary']        ?? '');
    $rawResponse    = $conn->real_escape_string($aiData['raw_response']   ?? '');
    $periodStr      = $conn->real_escape_string($period);
    $avgVoltage     = $stats['avg_voltage'];
    $avgPower       = $stats['avg_power'];
    $todayEnergy    = $stats['today_energy'];
    $avgPf          = $stats['avg_pf'];

    $conn->query("INSERT INTO ai_recommendations
        (generated_at, period, energy_rating, rating_score, predicted_bill,
         recommendations, efficiency_tip, summary, raw_response,
         avg_voltage, avg_power, today_energy, avg_pf)
        VALUES
        ('$generatedAt', '$periodStr', '$energyRating', $ratingScore, $predictedBill,
         '$recommendations', '$efficiencyTip', '$summary', '$rawResponse',
         $avgVoltage, $avgPower, $todayEnergy, $avgPf)");
}

/**
 * Return the ISO-8601 datetime of the next scheduled generation window.
 * Windows are 06:00 and 18:00 (Philippine Time).
 */
function getNextGenerationTime(DateTime $now): string {
    $hour = (int)$now->format('H');
    $next = clone $now;
    if ($hour < 6) {
        $next->setTime(6, 0, 0);
    } elseif ($hour < 18) {
        $next->setTime(18, 0, 0);
    } else {
        $next->modify('+1 day')->setTime(6, 0, 0);
    }
    return $next->format('Y-m-d H:i:s');
}
