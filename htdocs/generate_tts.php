<?php
/**
 * Generates BMO-personality TTS audio from the latest AI recommendation.
 * Call via cron at 6:00 AM and 6:00 PM, or trigger manually from a browser.
 *
 * Outputs: tts_recommend.wav  — the audio file
 *          tts_version.txt    — timestamp used by ESP32 for cache comparison
 *          tts_bmo_text.txt   — the BMO-rewritten text (for debugging)
 */
header('Content-Type: application/json');
set_time_limit(120);
ignore_user_abort(true);

$env       = parse_ini_file(__DIR__ . '/.env');
$geminiKey = $env['GEMINI_API_KEY'] ?? '';
if (!$geminiKey) {
    echo json_encode(['error' => 'GEMINI_API_KEY not set in .env']);
    exit;
}

// ── 1. Fetch latest recommendation from DB ──────────────────────────────────
try {
    $pdo = new PDO(
        "mysql:host={$env['DB_HOST']};dbname={$env['DB_NAME']};charset=utf8",
        $env['DB_USER'], $env['DB_PASS']
    );
    $pdo->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

    $row = $pdo->query("
        SELECT energy_rating, rating_score, recommendations,
               efficiency_tip, predicted_bill, generated_at
        FROM ai_recommendations
        ORDER BY generated_at DESC
        LIMIT 1
    ")->fetch(PDO::FETCH_ASSOC);
} catch (Exception $e) {
    echo json_encode(['error' => 'DB: ' . $e->getMessage()]);
    exit;
}

if (!$row) {
    echo json_encode(['error' => 'No recommendations found in DB']);
    exit;
}

// ── Smart cache: skip Gemini if recommendation hasn't changed ───────────────
$recVersion = 'db:' . $row['generated_at'];
$cachedVer  = trim(@file_get_contents(__DIR__ . '/tts_version.txt') ?: '');
$noCache    = isset($_GET['nocache']) && $_GET['nocache'] === '1';
if (!$noCache && $cachedVer === $recVersion && file_exists(__DIR__ . '/tts_recommend.wav')) {
    echo json_encode(['status' => 'cached', 'version' => $recVersion]);
    exit;
}

$recs    = json_decode($row['recommendations'], true);
$recList = (is_array($recs) && count($recs) > 0) ? implode(' ', $recs) : '';
$tip     = trim($row['efficiency_tip'] ?? '');
$summary = trim($row['summary'] ?? '');

// Build observation text from whatever is available
$allRecs = $recList ?: $tip ?: $summary;
if (!$allRecs) {
    echo json_encode(['error' => 'Recommendation row has no usable content (recs/tip/summary all empty)']);
    exit;
}

// ── 2. Rewrite in BMO's voice via Gemini ────────────────────────────────────
$extraTip = ($tip && $tip !== $allRecs) ? " Also: {$tip}" : '';
$bmoPrompt = <<<PROMPT
Rewrite the energy update below as if BMO from Adventure Time is saying it out loud — cheerful, childlike, a little silly, but genuinely helpful and specific. Output ONLY the spoken words BMO would say. No labels, no markdown, no asterisks, no formatting. Just the speech, 60 to 90 words, in first person as BMO. Do not mention any rating, score, or bill amount.

Energy update to rewrite: {$allRecs}{$extraTip}
PROMPT;

$textPayload = json_encode([
    'contents'         => [['parts' => [['text' => $bmoPrompt]]]],
    'generationConfig' => ['maxOutputTokens' => 4096, 'temperature' => 1.0],
]);

$ch = curl_init("https://generativelanguage.googleapis.com/v1beta/models/gemini-flash-latest:generateContent?key={$geminiKey}");
curl_setopt_array($ch, [
    CURLOPT_POST           => true,
    CURLOPT_POSTFIELDS     => $textPayload,
    CURLOPT_HTTPHEADER     => ['Content-Type: application/json'],
    CURLOPT_RETURNTRANSFER => true,
    CURLOPT_TIMEOUT        => 30,
    CURLOPT_SSL_VERIFYPEER => false,
]);
$textResp = curl_exec($ch);
$curlErr  = curl_error($ch);
curl_close($ch);

if ($curlErr) {
    echo json_encode(['error' => 'Text gen cURL: ' . $curlErr]);
    exit;
}

$textData    = json_decode($textResp, true);
$parts       = $textData['candidates'][0]['content']['parts'] ?? [];
$bmoText     = trim(implode('', array_column($parts, 'text')));
$finishReason= $textData['candidates'][0]['finishReason'] ?? 'unknown';
if (!$bmoText) {
    echo json_encode(['error' => 'Gemini text generation returned empty', 'finishReason' => $finishReason, 'raw' => substr($textResp, 0, 500)]);
    exit;
}
// Debug: expose generation metadata so truncation can be diagnosed
$_debugText = ['word_count' => str_word_count($bmoText), 'finish_reason' => $finishReason];

// ── 3. Generate TTS with voice style marker ─────────────────────────────────
$voiceText = "[A playful robot like BMO from Adventure Time, a very happy robot] {$bmoText}";

$ttsPayload = json_encode([
    'contents'         => [['parts' => [['text' => $voiceText]]]],
    'generationConfig' => [
        'responseModalities' => ['AUDIO'],
        'speechConfig'       => [
            'voiceConfig' => [
                'prebuiltVoiceConfig' => ['voiceName' => 'Laomedeia'],
            ],
        ],
    ],
]);

$ch = curl_init("https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash-preview-tts:generateContent?key={$geminiKey}");
curl_setopt_array($ch, [
    CURLOPT_POST           => true,
    CURLOPT_POSTFIELDS     => $ttsPayload,
    CURLOPT_HTTPHEADER     => ['Content-Type: application/json'],
    CURLOPT_RETURNTRANSFER => true,
    CURLOPT_TIMEOUT        => 90,
    CURLOPT_SSL_VERIFYPEER => false,
]);
$ttsResp = curl_exec($ch);
$curlErr = curl_error($ch);
curl_close($ch);

if ($curlErr) {
    echo json_encode(['error' => 'TTS cURL: ' . $curlErr]);
    exit;
}

$ttsData  = json_decode($ttsResp, true);
$ttsParts = $ttsData['candidates'][0]['content']['parts'] ?? [];

// Decode each part's binary separately, then concatenate the raw PCM.
// Do NOT concatenate base64 strings — they must be decoded individually.
$pcm = '';
foreach ($ttsParts as $p) {
    if (!empty($p['inlineData']['data'])) {
        $pcm .= base64_decode($p['inlineData']['data']);
    }
}

if ($pcm === '') {
    echo json_encode(['error' => 'TTS returned no audio', 'raw' => substr($ttsResp, 0, 400)]);
    exit;
}

// ── 4. Build WAV and save to disk ───────────────────────────────────────────
$sampleRate = 24000;
$channels   = 1;
$bps        = 16;
$byteRate   = $sampleRate * $channels * $bps / 8;
$blockAlign = $channels * $bps / 8;
$dataLen    = strlen($pcm);
$chunkSize  = 36 + $dataLen;

$wav  = 'RIFF';
$wav .= pack('V', $chunkSize);
$wav .= 'WAVEfmt ';
$wav .= pack('V', 16);          // subchunk1 size (PCM)
$wav .= pack('v', 1);           // audio format: PCM
$wav .= pack('v', $channels);
$wav .= pack('V', $sampleRate);
$wav .= pack('V', $byteRate);
$wav .= pack('v', $blockAlign);
$wav .= pack('v', $bps);
$wav .= 'data';
$wav .= pack('V', $dataLen);
$wav .= $pcm;

file_put_contents(__DIR__ . '/tts_recommend.wav', $wav);
file_put_contents(__DIR__ . '/tts_version.txt',   $recVersion);  // "db:{generated_at}"
file_put_contents(__DIR__ . '/tts_bmo_text.txt',  $bmoText);

echo json_encode([
    'status'    => 'ok',
    'version'   => $recVersion,
    'bmo_text'  => $bmoText,
    'wav_bytes' => strlen($wav),
    'debug'     => $_debugText,
]);
