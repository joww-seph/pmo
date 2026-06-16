<?php
/**
 * nilm.php — Non-Intrusive Load Monitoring (NILM) API
 *
 * Provides three actions via ?action=<name>:
 *   list     — return all registered device signatures
 *   register — store a new device signature (averaged from 30-second sampling)
 *   detect   — identify which registered appliances are currently on
 *
 * NILM INFERENCE ALGORITHM (detect action)
 * -----------------------------------------
 * The detection problem: given live readings (power, current, PF) for the
 * whole circuit, determine which combination of registered appliances best
 * explains the total draw.
 *
 * Approach — exhaustive subset search:
 *   For n registered devices, there are 2^n − 1 non-empty subsets.
 *   Each subset is evaluated as a candidate "active set" by summing the
 *   power and current of its members and averaging their power factors.
 *
 * Scoring function (weighted Euclidean distance):
 *   score = sqrt( (P_live − ΣP_devices)²
 *               + (I_live − ΣI_devices)² × 10
 *               + (PF_live − avgPF_devices)² × 5 )
 *
 *   Current gets weight 10 and PF weight 5 because current and PF provide
 *   stronger discrimination between appliance types than raw wattage alone.
 *
 * Match threshold: score < 60 is considered a positive identification.
 * Confidence: confidence = max(0, (1 − score / 60) × 100) %
 *
 * Complexity: O(2^n) — practical only for small device libraries (n ≤ 20).
 */

header("Content-Type: application/json");
header("Access-Control-Allow-Origin: *");
header("Access-Control-Allow-Methods: POST, GET");
header("Access-Control-Allow-Headers: Content-Type");

require_once __DIR__ . '/config.php';

$action = $_GET['action'] ?? '';

if ($action === 'list') {
    // Return all stored appliance signatures, newest first.
    $rows = [];
    $res = $conn->query("SELECT * FROM device_signatures ORDER BY created_at DESC");
    while ($row = $res->fetch_assoc()) { $rows[] = $row; }
    echo json_encode($rows);

} elseif ($action === 'register') {
    // Store the 30-second averaged signature for a new appliance.
    // The frontend captures samples every second and sends the mean after 30 s.
    $body = json_decode(file_get_contents("php://input"), true);
    $stmt = $conn->prepare("INSERT INTO device_signatures
        (device_name, avg_power, avg_current, avg_pf, avg_apparent, delta_power, delta_current)
        VALUES (?, ?, ?, ?, ?, ?, ?)");
    $stmt->bind_param("sdddddd",
        $body['device_name'],
        $body['avg_power'],
        $body['avg_current'],
        $body['avg_pf'],
        $body['avg_apparent'],
        $body['delta_power'],   // delta = value-with-device minus baseline (no device)
        $body['delta_current']
    );
    $stmt->execute();
    echo json_encode(['status' => 'registered']);

} elseif ($action === 'detect') {
    // ── Step 1: Read live circuit measurements from the POST body ──────────
    $body = json_decode(file_get_contents("php://input"), true);
    $pw   = floatval($body['power']);
    $curr = floatval($body['current']);
    $pf   = floatval($body['pf']);

    // ── Step 2: Load all registered device signatures from the database ───
    $sigs = [];
    $res = $conn->query("SELECT * FROM device_signatures");
    while ($row = $res->fetch_assoc()) { $sigs[] = $row; }

    if (empty($sigs)) {
        echo json_encode(['matches' => [], 'score' => 0]);
        $conn->close();
        exit;
    }

    // ── Step 3: Exhaustive bitmask search over all non-empty subsets ───────
    // Each integer $mask from 1 to 2^n − 1 represents one candidate subset.
    // Bit i of $mask = 1 means device i is included in this candidate set.
    $n         = count($sigs);
    $bestScore = INF;
    $bestCombo = [];

    for ($mask = 1; $mask < (1 << $n); $mask++) {
        $sumPower   = 0;
        $sumCurrent = 0;
        $sumPF      = 0;
        $count      = 0;
        $combo      = [];

        // Accumulate the power/current/PF of every device selected by this mask.
        for ($i = 0; $i < $n; $i++) {
            if ($mask & (1 << $i)) {
                $sumPower   += floatval($sigs[$i]['avg_power']);
                $sumCurrent += floatval($sigs[$i]['avg_current']);
                $sumPF      += floatval($sigs[$i]['avg_pf']);
                $count++;
                $combo[]     = $sigs[$i];
            }
        }

        $avgPF = $sumPF / $count;

        // Weighted Euclidean distance between the live reading and this candidate set.
        // Higher weights on current and PF improve discrimination between
        // appliances that share similar wattage (e.g. a lamp vs. a fan).
        $score = sqrt(
            pow($pw   - $sumPower,   2) +
            pow($curr - $sumCurrent, 2) * 10 +
            pow($pf   - $avgPF,      2) * 5
        );

        // Keep the candidate set with the smallest distance to the live reading.
        if ($score < $bestScore) {
            $bestScore = $score;
            $bestCombo = $combo;
        }
    }

    // ── Step 4: Apply threshold and calculate confidence ──────────────────
    // A score below 60 means the best-matching subset is close enough to the
    // live reading to be reported as a positive identification.
    $THRESHOLD = 60;
    $matches   = [];

    if ($bestScore < $THRESHOLD) {
        // Map score linearly to a 0–100 % confidence scale.
        $confidence = max(0, round((1 - $bestScore / $THRESHOLD) * 100, 1));
        foreach ($bestCombo as $dev) {
            $matches[] = [
                'device'    => $dev['device_name'],
                'avg_power' => $dev['avg_power']
            ];
        }
        // Log the detection event so trends can be analysed over time.
        $names = implode(', ', array_column($bestCombo, 'device_name'));
        $stmt  = $conn->prepare("INSERT INTO nilm_detections (matched_device, confidence) VALUES (?, ?)");
        $stmt->bind_param("sd", $names, $confidence);
        $stmt->execute();

        echo json_encode([
            'matches'    => $matches,
            'confidence' => $confidence,
            'score'      => $bestScore
        ]);
    } else {
        // No subset scored below the threshold — circuit state is unrecognised.
        echo json_encode(['matches' => [], 'confidence' => 0, 'score' => $bestScore]);
    }
}

$conn->close();
