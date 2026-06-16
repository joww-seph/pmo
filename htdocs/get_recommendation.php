<?php
header('Content-Type: application/json');
header('Access-Control-Allow-Origin: *');

$env  = parse_ini_file(__DIR__ . '/.env');
$host = $env['DB_HOST'];
$db   = $env['DB_NAME'];
$user = $env['DB_USER'];
$pass = $env['DB_PASS'];

try {
    $pdo = new PDO("mysql:host=$host;dbname=$db;charset=utf8", $user, $pass);
    $pdo->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

    $row = $pdo->query("
        SELECT
            energy_rating,
            rating_score,
            recommendations,
            efficiency_tip,
            predicted_bill
        FROM ai_recommendations
        ORDER BY generated_at DESC
        LIMIT 1
    ")->fetch(PDO::FETCH_ASSOC);

    if (!$row) {
        echo json_encode(['error' => 'No recommendations found']);
        exit;
    }

    $bill = number_format((float)$row['predicted_bill'], 2);

    // Use only the first recommendation to keep the spoken text short (~10 s)
    $recs     = json_decode($row['recommendations'], true);
    $firstRec = (is_array($recs) && count($recs) > 0) ? $recs[0] : $row['efficiency_tip'];

    $spoken  = "Energy rating {$row['energy_rating']}, score {$row['rating_score']} out of 100. ";
    $spoken .= "{$firstRec} ";
    $spoken .= "Predicted bill: {$bill} pesos.";

    echo json_encode([
        'spoken' => $spoken,
        'rating' => $row['energy_rating'],
        'score'  => $row['rating_score'],
        'bill'   => $bill,
        'tips'   => $row['efficiency_tip'],
    ]);

} catch (PDOException $e) {
    echo json_encode(['error' => $e->getMessage()]);
}
