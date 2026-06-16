<?php
header('Content-Type: application/json');
header('Access-Control-Allow-Origin: *');

$version  = trim(@file_get_contents(__DIR__ . '/tts_version.txt')  ?: '');
$bmoText  = trim(@file_get_contents(__DIR__ . '/tts_bmo_text.txt') ?: '');
$hasAudio = file_exists(__DIR__ . '/tts_recommend.wav');

echo json_encode([
    'version'   => $version,
    'text'      => $bmoText,
    'has_audio' => $hasAudio,
]);
