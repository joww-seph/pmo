<?php
$file = __DIR__ . '/tts_recommend.wav';

if (!file_exists($file)) {
    http_response_code(404);
    header('Content-Type: application/json');
    echo json_encode(['error' => 'No TTS audio has been generated yet. Call generate_tts.php first.']);
    exit;
}

header('Content-Type: audio/wav');
header('Content-Length: ' . filesize($file));
header('Content-Disposition: attachment; filename="tts_recommend.wav"');
header('Cache-Control: no-store');
readfile($file);
