/*
 * SmartHouse ESP32-CAM  -  Network Camera (Web Streaming Server)
 * ---------------------------------------------------------------
 * Board   : AI-Thinker ESP32-CAM
 * Core    : esp32 by Espressif  2.0.7 (Arduino IDE)
 *
 * Features:
 *   - WiFi STA connection
 *   - Live MJPEG stream            http://<ip>/stream
 *   - Single JPEG snapshot         http://<ip>/capture
 *   - Flash LED control (GPIO 4)   http://<ip>/led?state=on|off|toggle
 *   - Camera control/status        http://<ip>/control?var=<name>&val=<v>
 *                                  http://<ip>/status
 *   - Modern responsive web UI     http://<ip>/
 *
 * Serial Monitor: 115200 baud
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include "esp_http_server.h"

// =====================  USER CONFIG  =====================
const char *WIFI_SSID = "abc";
const char *WIFI_PASS = "123456789b";

const uint32_t WIFI_TIMEOUT_MS = 20000;
// =========================================================

// ==============  AI-THINKER ESP32-CAM PIN MAP  ============
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#define FLASH_LED_GPIO     4
// =========================================================

httpd_handle_t camera_httpd = NULL;

// ---------------------------------------------------------
//  Camera initialisation
// ---------------------------------------------------------
static bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size   = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_count     = 2;
    config.grab_mode    = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size   = FRAMESIZE_QVGA;
    config.jpeg_quality = 15;
    config.fb_count     = 1;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] init failed: 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s != NULL) {
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1);
    s->set_gain_ctrl(s, 1);
    s->set_hmirror(s, 0);
    s->set_vflip(s, 0);
  }

  Serial.println("[CAM] initialised OK");
  return true;
}

// ---------------------------------------------------------
//  MJPEG stream handler  ->  /stream
// ---------------------------------------------------------
#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  size_t jpg_buf_len = 0;
  uint8_t *jpg_buf = NULL;
  char part_buf[64];

  res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");
  httpd_resp_set_hdr(req, "Connection", "close");

  Serial.println("[STREAM] client connected");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("[STREAM] frame grab failed");
      res = ESP_FAIL;
      break;
    }

    if (fb->format != PIXFORMAT_JPEG) {
      bool ok = frame2jpg(fb, 80, &jpg_buf, &jpg_buf_len);
      esp_camera_fb_return(fb);
      fb = NULL;
      if (!ok) {
        Serial.println("[STREAM] frame2jpg failed");
        res = ESP_FAIL;
        break;
      }
    } else {
      jpg_buf_len = fb->len;
      jpg_buf     = fb->buf;
    }

    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART,
                             (unsigned)jpg_buf_len);
      res = httpd_resp_send_chunk(req, STREAM_BOUNDARY,
                                  strlen(STREAM_BOUNDARY));
      if (res == ESP_OK)
        res = httpd_resp_send_chunk(req, part_buf, hlen);
      if (res == ESP_OK)
        res = httpd_resp_send_chunk(req, (const char *)jpg_buf, jpg_buf_len);
    }

    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
      jpg_buf = NULL;
    } else if (jpg_buf) {
      free(jpg_buf);
      jpg_buf = NULL;
    }

    if (res != ESP_OK) break;
  }

  Serial.println("[STREAM] client disconnected");
  return res;
}

// ---------------------------------------------------------
//  Single capture handler  ->  /capture
// ---------------------------------------------------------
static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[CAPTURE] frame grab failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition",
                     "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

// ---------------------------------------------------------
//  Flash LED handler  ->  /led?state=on|off|toggle
// ---------------------------------------------------------
static esp_err_t led_handler(httpd_req_t *req) {
  char query[32] = {0};
  char value[8]  = {0};
  int ledState = digitalRead(FLASH_LED_GPIO);

  if (httpd_req_get_url_query_len(req) > 0) {
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
      if (httpd_query_key_value(query, "state", value, sizeof(value)) ==
          ESP_OK) {
        if (!strcmp(value, "on"))          ledState = 1;
        else if (!strcmp(value, "off"))    ledState = 0;
        else if (!strcmp(value, "toggle")) ledState = !ledState;
      }
    }
  }
  digitalWrite(FLASH_LED_GPIO, ledState);

  char json[40];
  snprintf(json, sizeof(json), "{\"led\":\"%s\"}", ledState ? "on" : "off");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json, strlen(json));
}

// ---------------------------------------------------------
//  Camera control handler  ->  /control?var=NAME&val=VALUE
// ---------------------------------------------------------
static esp_err_t control_handler(httpd_req_t *req) {
  char query[64]  = {0};
  char var[24]    = {0};
  char valStr[16] = {0};
  int val = 0;
  sensor_t *s = esp_camera_sensor_get();
  int ret = 0;

  if (s == NULL) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  if (httpd_req_get_url_query_len(req) == 0 ||
      httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "var", var, sizeof(var)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'var'");
    return ESP_FAIL;
  }
  httpd_query_key_value(query, "val", valStr, sizeof(valStr));
  val = atoi(valStr);

  if      (!strcmp(var, "framesize")) {
    if (s->pixformat == PIXFORMAT_JPEG) ret = s->set_framesize(s, (framesize_t)val);
  }
  else if (!strcmp(var, "quality"))       ret = s->set_quality(s, val);
  else if (!strcmp(var, "brightness"))    ret = s->set_brightness(s, val);
  else if (!strcmp(var, "contrast"))      ret = s->set_contrast(s, val);
  else if (!strcmp(var, "saturation"))    ret = s->set_saturation(s, val);
  else if (!strcmp(var, "sharpness"))     ret = s->set_sharpness(s, val);
  else if (!strcmp(var, "denoise"))       ret = s->set_denoise(s, val);
  else if (!strcmp(var, "gainceiling"))   ret = s->set_gainceiling(s, (gainceiling_t)val);
  else if (!strcmp(var, "colorbar"))      ret = s->set_colorbar(s, val);
  else if (!strcmp(var, "whitebal"))      ret = s->set_whitebal(s, val);
  else if (!strcmp(var, "awb_gain"))      ret = s->set_awb_gain(s, val);
  else if (!strcmp(var, "exposure_ctrl")) ret = s->set_exposure_ctrl(s, val);
  else if (!strcmp(var, "aec2"))          ret = s->set_aec2(s, val);
  else if (!strcmp(var, "ae_level"))      ret = s->set_ae_level(s, val);
  else if (!strcmp(var, "aec_value"))     ret = s->set_aec_value(s, val);
  else if (!strcmp(var, "gain_ctrl"))     ret = s->set_gain_ctrl(s, val);
  else if (!strcmp(var, "agc_gain"))      ret = s->set_agc_gain(s, val);
  else if (!strcmp(var, "hmirror"))       ret = s->set_hmirror(s, val);
  else if (!strcmp(var, "vflip"))         ret = s->set_vflip(s, val);
  else if (!strcmp(var, "dcw"))           ret = s->set_dcw(s, val);
  else if (!strcmp(var, "bpc"))           ret = s->set_bpc(s, val);
  else if (!strcmp(var, "wpc"))           ret = s->set_wpc(s, val);
  else if (!strcmp(var, "raw_gma"))       ret = s->set_raw_gma(s, val);
  else if (!strcmp(var, "lenc"))          ret = s->set_lenc(s, val);
  else if (!strcmp(var, "special_effect"))ret = s->set_special_effect(s, val);
  else if (!strcmp(var, "wb_mode"))       ret = s->set_wb_mode(s, val);
  else {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown 'var'");
    return ESP_FAIL;
  }

  char json[48];
  snprintf(json, sizeof(json), "{\"ok\":%d,\"var\":\"%s\"}", ret, var);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json, strlen(json));
}

// ---------------------------------------------------------
//  Status handler  ->  /status  (current settings as JSON)
// ---------------------------------------------------------
static esp_err_t status_handler(httpd_req_t *req) {
  sensor_t *s = esp_camera_sensor_get();
  char json[512];

  if (s == NULL) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  snprintf(json, sizeof(json),
    "{"
      "\"led\":%d,"
      "\"framesize\":%d,"
      "\"quality\":%d,"
      "\"brightness\":%d,"
      "\"contrast\":%d,"
      "\"saturation\":%d,"
      "\"sharpness\":%d,"
      "\"hmirror\":%d,"
      "\"vflip\":%d,"
      "\"whitebal\":%d,"
      "\"awb_gain\":%d,"
      "\"exposure_ctrl\":%d,"
      "\"aec2\":%d,"
      "\"gain_ctrl\":%d,"
      "\"ae_level\":%d,"
      "\"special_effect\":%d,"
      "\"wb_mode\":%d,"
      "\"rssi\":%d"
    "}",
    digitalRead(FLASH_LED_GPIO),
    s->status.framesize,
    s->status.quality,
    s->status.brightness,
    s->status.contrast,
    s->status.saturation,
    s->status.sharpness,
    s->status.hmirror,
    s->status.vflip,
    s->status.awb,
    s->status.awb_gain,
    s->status.aec,
    s->status.aec2,
    s->status.agc,
    s->status.ae_level,
    s->status.special_effect,
    s->status.wb_mode,
    (int)WiFi.RSSI());

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json, strlen(json));
}

// ---------------------------------------------------------
//  Root HTML page  ->  /
// ---------------------------------------------------------
static const char INDEX_HTML[] PROGMEM = R"HTMLPAGE(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>ESP32-CAM 智能家居摄像头</title>
<style>
  :root{
    --bg:#0b0f14; --panel:#151c26; --panel2:#1b2432; --line:#26313f;
    --txt:#e8eef7; --dim:#8fa0b4; --accent:#2d7ef7; --accent2:#00c2a8;
    --danger:#ff4d5e; --ok:#28c76f; --shadow:0 10px 30px rgba(0,0,0,.45);
    --radius:16px;
  }
  *{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
  html,body{margin:0;height:100%}
  body{
    font-family:-apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC",
      "Microsoft YaHei",Roboto,Helvetica,Arial,sans-serif;
    background:radial-gradient(1200px 600px at 50% -10%,#16202c 0%,var(--bg) 60%);
    color:var(--txt);min-height:100%;
  }
  .wrap{max-width:1200px;margin:0 auto;padding:20px 16px 48px}

  header{display:flex;align-items:center;gap:12px;margin-bottom:18px;flex-wrap:wrap}
  .logo{width:38px;height:38px;border-radius:11px;flex:0 0 auto;
    background:linear-gradient(135deg,var(--accent),var(--accent2));
    display:grid;place-items:center;box-shadow:var(--shadow)}
  .logo svg{width:20px;height:20px;fill:#fff}
  header h1{font-size:1.15rem;margin:0;font-weight:650;letter-spacing:.2px}
  header .sub{font-size:.78rem;color:var(--dim);margin-top:2px}
  .spacer{flex:1}
  .pill{display:inline-flex;align-items:center;gap:7px;font-size:.76rem;
    padding:6px 12px;border-radius:999px;background:var(--panel);
    border:1px solid var(--line);color:var(--dim)}
  .dot{width:8px;height:8px;border-radius:50%;background:var(--danger);
    box-shadow:0 0 8px var(--danger);transition:.3s}
  .dot.on{background:var(--ok);box-shadow:0 0 8px var(--ok)}

  .stage{position:relative;border-radius:var(--radius);overflow:hidden;
    background:#000;border:1px solid var(--line);box-shadow:var(--shadow);
    aspect-ratio:4/3}
  .stage img,.stage canvas{width:100%;height:100%;object-fit:contain;
    display:block;background:#000}
  .stage canvas{position:absolute;inset:0;display:none}
  .stage.off .placeholder{display:grid}
  .placeholder{position:absolute;inset:0;display:none;place-items:center;
    text-align:center;color:var(--dim);background:#0d1218}
  .placeholder svg{width:56px;height:56px;fill:#33414f;margin-bottom:10px}
  .badge{position:absolute;top:12px;left:12px;display:flex;gap:8px;z-index:3}
  .tag{font-size:.72rem;padding:5px 10px;border-radius:8px;
    background:rgba(10,14,20,.72);border:1px solid var(--line);
    backdrop-filter:blur(6px);color:var(--dim)}
  .tag.rec{color:#fff;background:rgba(255,77,94,.9);border-color:transparent}
  .tag.rec::before{content:"";display:inline-block;width:8px;height:8px;
    border-radius:50%;background:#fff;margin-right:6px;animation:blink 1s infinite}
  @keyframes blink{50%{opacity:.25}}
  .loader{position:absolute;inset:0;display:grid;place-items:center;
    background:#0d1218;color:var(--dim);gap:14px;z-index:2}
  .spin{width:34px;height:34px;border-radius:50%;border:3px solid #2b3846;
    border-top-color:var(--accent);animation:spin .9s linear infinite;margin:0 auto 12px}
  @keyframes spin{to{transform:rotate(360deg)}}
  .hidden{display:none!important}

  .bar{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));
    gap:10px;margin-top:14px}
  button.act{display:flex;align-items:center;justify-content:center;gap:8px;
    padding:13px 14px;font-size:.92rem;font-weight:600;color:var(--txt);
    background:var(--panel);border:1px solid var(--line);border-radius:13px;
    cursor:pointer;transition:.15s;user-select:none}
  button.act:hover{background:var(--panel2);transform:translateY(-1px)}
  button.act:active{transform:translateY(0) scale(.98)}
  button.act svg{width:18px;height:18px;fill:currentColor}
  button.act.primary{background:linear-gradient(135deg,var(--accent),#1f66d0);
    border-color:transparent}
  button.act.warn.on{background:linear-gradient(135deg,#ffb020,#f08a00);
    border-color:transparent;color:#20160a}
  button.act.rec.on{background:linear-gradient(135deg,#ff4d5e,#d92b3c);
    border-color:transparent;color:#fff}
  button.act.act-on{background:linear-gradient(135deg,var(--accent2),#009e89);
    border-color:transparent;color:#04241f}

  .grid{display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-top:16px}
  @media(max-width:760px){.grid{grid-template-columns:1fr}}
  .card{background:var(--panel);border:1px solid var(--line);
    border-radius:var(--radius);padding:16px 16px 18px;box-shadow:var(--shadow)}
  .card h2{font-size:.82rem;text-transform:uppercase;letter-spacing:.8px;
    color:var(--dim);margin:0 0 14px;font-weight:600;display:flex;
    align-items:center;gap:8px}
  .card h2 svg{width:15px;height:15px;fill:var(--accent)}

  .field{margin-bottom:14px}
  .field:last-child{margin-bottom:0}
  .field label{display:flex;justify-content:space-between;font-size:.84rem;
    color:var(--txt);margin-bottom:7px;font-weight:500}
  .field label span{color:var(--accent);font-variant-numeric:tabular-nums}
  input[type=range]{-webkit-appearance:none;appearance:none;width:100%;
    height:6px;border-radius:999px;background:#2a3542;outline:none}
  input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;
    width:18px;height:18px;border-radius:50%;background:var(--accent);
    border:3px solid #0f1620;box-shadow:0 2px 6px rgba(0,0,0,.5);cursor:pointer}
  input[type=range]::-moz-range-thumb{width:18px;height:18px;border-radius:50%;
    background:var(--accent);border:3px solid #0f1620;cursor:pointer}
  select{width:100%;padding:10px 12px;border-radius:10px;background:var(--panel2);
    color:var(--txt);border:1px solid var(--line);font-size:.9rem;outline:none}
  select:focus{border-color:var(--accent)}

  .switches{display:grid;grid-template-columns:1fr 1fr;gap:10px}
  .switch{display:flex;align-items:center;justify-content:space-between;
    padding:10px 12px;border-radius:11px;background:var(--panel2);
    border:1px solid var(--line);font-size:.85rem;cursor:pointer}
  .switch input{display:none}
  .track{width:38px;height:22px;border-radius:999px;background:#37444f;
    position:relative;transition:.2s;flex:0 0 auto}
  .track::after{content:"";position:absolute;top:3px;left:3px;width:16px;
    height:16px;border-radius:50%;background:#fff;transition:.2s}
  .switch input:checked + .track{background:var(--accent2)}
  .switch input:checked + .track::after{transform:translateX(16px)}

  .gallery{display:grid;grid-template-columns:repeat(auto-fill,minmax(110px,1fr));
    gap:10px;margin-top:4px}
  .shot{position:relative;border-radius:10px;overflow:hidden;
    border:1px solid var(--line);background:#0d1218;aspect-ratio:4/3}
  .shot img{width:100%;height:100%;object-fit:cover;display:block}
  .shot button{position:absolute;top:5px;right:5px;width:24px;height:24px;
    border-radius:7px;border:0;background:rgba(10,14,20,.75);color:#fff;
    cursor:pointer;font-size:14px;line-height:1;display:grid;place-items:center}
  .empty{color:var(--dim);font-size:.82rem;padding:14px 0;text-align:center}

  .toast{position:fixed;left:50%;bottom:26px;transform:translateX(-50%) translateY(20px);
    background:#1e2836;border:1px solid var(--line);color:var(--txt);
    padding:12px 20px;border-radius:12px;box-shadow:var(--shadow);font-size:.88rem;
    opacity:0;pointer-events:none;transition:.28s;z-index:99;max-width:90vw;text-align:center}
  .toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
  footer{text-align:center;color:var(--dim);font-size:.76rem;margin-top:26px}
</style>
</head>
<body>
<div class="wrap">

  <header>
    <div class="logo">
      <svg viewBox="0 0 24 24"><path d="M17 10.5V7a2 2 0 0 0-2-2H4a2 2 0 0 0-2 2v10a2 2 0 0 0 2 2h11a2 2 0 0 0 2-2v-3.5l5 3.5V7l-5 3.5z"/></svg>
    </div>
    <div>
      <h1>智能家居摄像头</h1>
      <div class="sub">ESP32-CAM · AI-Thinker</div>
    </div>
    <div class="spacer"></div>
    <div class="pill"><span class="dot" id="netDot"></span><span id="netText">连接中…</span></div>
  </header>

  <div class="stage" id="stage">
    <div class="badge">
      <span class="tag" id="resTag">—</span>
      <span class="tag hidden" id="recTag">录制中</span>
    </div>
    <div class="loader" id="loader">
      <div style="text-align:center">
        <div class="spin"></div>
        <div>正在启动视频流…</div>
      </div>
    </div>
    <img id="video" alt="实时画面">
    <canvas id="canvas"></canvas>
    <div class="placeholder" id="placeholder">
      <div>
        <svg viewBox="0 0 24 24"><path d="M17 10.5V7a2 2 0 0 0-2-2H4a2 2 0 0 0-2 2v10a2 2 0 0 0 2 2h11a2 2 0 0 0 2-2v-3.5l5 3.5V7l-5 3.5z"/></svg>
        <div>视频已关闭</div>
      </div>
    </div>
  </div>

  <div class="bar">
    <button class="act primary" id="btnVideo" onclick="toggleVideo()">
      <svg viewBox="0 0 24 24"><path d="M17 10.5V7a2 2 0 0 0-2-2H4a2 2 0 0 0-2 2v10a2 2 0 0 0 2 2h11a2 2 0 0 0 2-2v-3.5l5 3.5V7l-5 3.5z"/></svg>
      <span>关闭视频</span>
    </button>
    <button class="act" id="btnShot" onclick="takePhoto()">
      <svg viewBox="0 0 24 24"><path d="M12 15.2a3.2 3.2 0 1 0 0-6.4 3.2 3.2 0 0 0 0 6.4zM9 2 7.2 4H4a2 2 0 0 0-2 2v12a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2V6a2 2 0 0 0-2-2h-3.2L15 2H9z"/></svg>
      <span>拍照</span>
    </button>
    <button class="act warn" id="btnLed" onclick="toggleLed()">
      <svg viewBox="0 0 24 24"><path d="M9 21h6v-1H9v1zm3-19a7 7 0 0 0-4 12.7V17a1 1 0 0 0 1 1h6a1 1 0 0 0 1-1v-2.3A7 7 0 0 0 12 2z"/></svg>
      <span>补光灯</span>
    </button>
    <button class="act rec" id="btnRec" onclick="toggleRec()">
      <svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="8"/></svg>
      <span>开始录制</span>
    </button>
    <button class="act" onclick="fullscreen()">
      <svg viewBox="0 0 24 24"><path d="M7 14H5v5h5v-2H7v-3zm-2-4h2V7h3V5H5v5zm12 7h-3v2h5v-5h-2v3zM14 5v2h3v3h2V5h-5z"/></svg>
      <span>全屏</span>
    </button>
  </div>

  <div class="grid">
    <div class="card">
      <h2><svg viewBox="0 0 24 24"><path d="M3 17v2h6v-2H3zM3 5v2h10V5H3zm10 16v-2h8v-2h-8v-2h-2v6h2zM7 9v2H3v2h4v2h2V9H7zm14 4v-2H11v2h10zm-6-4h2V7h4V5h-4V3h-2v6z"/></svg>画面调节</h2>

      <div class="field">
        <label>分辨率 <span id="vRes">VGA</span></label>
        <select id="selRes" onchange="setRes(this.value)">
          <option value="5">QVGA 320×240</option>
          <option value="8">VGA 640×480</option>
          <option value="9">SVGA 800×600</option>
          <option value="10">XGA 1024×768</option>
          <option value="11">HD 1280×720</option>
          <option value="12">SXGA 1280×1024</option>
          <option value="13">UXGA 1600×1200</option>
        </select>
      </div>
      <div class="field">
        <label>画质 (JPEG) <span id="vQual">12</span></label>
        <input type="range" id="rQual" min="4" max="40" value="12"
               oninput="ctl('quality',this.value,'vQual')">
      </div>
      <div class="field">
        <label>亮度 <span id="vBri">0</span></label>
        <input type="range" id="rBri" min="-2" max="2" value="0"
               oninput="ctl('brightness',this.value,'vBri')">
      </div>
      <div class="field">
        <label>对比度 <span id="vCon">0</span></label>
        <input type="range" id="rCon" min="-2" max="2" value="0"
               oninput="ctl('contrast',this.value,'vCon')">
      </div>
      <div class="field">
        <label>饱和度 <span id="vSat">0</span></label>
        <input type="range" id="rSat" min="-2" max="2" value="0"
               oninput="ctl('saturation',this.value,'vSat')">
      </div>
    </div>

    <div class="card">
      <h2><svg viewBox="0 0 24 24"><path d="M12 8a4 4 0 1 0 0 8 4 4 0 0 0 0-8zm8.9 3-1.9-.3a7 7 0 0 0-.8-2l1.2-1.5-1.6-1.6-1.5 1.2a7 7 0 0 0-2-.8L13.9 4h-3.8l-.3 1.9a7 7 0 0 0-2 .8L6.3 5.5 4.7 7.1l1.2 1.5a7 7 0 0 0-.8 2L3.1 11v3.8l1.9.3a7 7 0 0 0 .8 2l-1.2 1.5 1.6 1.6 1.5-1.2a7 7 0 0 0 2 .8l.3 1.9h3.8l.3-1.9a7 7 0 0 0 2-.8l1.5 1.2 1.6-1.6-1.2-1.5a7 7 0 0 0 .8-2l1.9-.3V11z"/></svg>更多设置</h2>

      <div class="field">
        <label>白平衡模式</label>
        <select id="selWb" onchange="ctl('wb_mode',this.value)">
          <option value="0">自动</option>
          <option value="1">晴天</option>
          <option value="2">阴天</option>
          <option value="3">办公室</option>
          <option value="4">居家</option>
        </select>
      </div>
      <div class="field">
        <label>特效滤镜</label>
        <select id="selFx" onchange="ctl('special_effect',this.value)">
          <option value="0">无</option>
          <option value="1">负片</option>
          <option value="2">灰度</option>
          <option value="3">偏红</option>
          <option value="4">偏绿</option>
          <option value="5">偏蓝</option>
          <option value="6">复古</option>
        </select>
      </div>
      <div class="field">
        <label>曝光补偿 <span id="vAe">0</span></label>
        <input type="range" id="rAe" min="-2" max="2" value="0"
               oninput="ctl('ae_level',this.value,'vAe')">
      </div>

      <div class="switches" style="margin-top:14px">
        <label class="switch">水平镜像
          <input type="checkbox" id="swH" onchange="ctl('hmirror',this.checked?1:0)"><span class="track"></span>
        </label>
        <label class="switch">垂直翻转
          <input type="checkbox" id="swV" onchange="ctl('vflip',this.checked?1:0)"><span class="track"></span>
        </label>
        <label class="switch">自动白平衡
          <input type="checkbox" id="swWb" checked onchange="ctl('whitebal',this.checked?1:0)"><span class="track"></span>
        </label>
        <label class="switch">自动曝光
          <input type="checkbox" id="swAe" checked onchange="ctl('exposure_ctrl',this.checked?1:0)"><span class="track"></span>
        </label>
        <label class="switch">自动增益
          <input type="checkbox" id="swAg" checked onchange="ctl('gain_ctrl',this.checked?1:0)"><span class="track"></span>
        </label>
        <label class="switch">彩条测试
          <input type="checkbox" id="swCb" onchange="ctl('colorbar',this.checked?1:0)"><span class="track"></span>
        </label>
      </div>
    </div>
  </div>

  <div class="card" style="margin-top:16px">
    <h2><svg viewBox="0 0 24 24"><path d="M21 19V5a2 2 0 0 0-2-2H5a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2zM8.5 13.5l2.5 3 3.5-4.5 4.5 6H5l3.5-4.5z"/></svg>相册 <span id="shotCount" style="color:var(--dim);font-weight:400;text-transform:none;letter-spacing:0"></span></h2>
    <div class="gallery" id="gallery"></div>
    <div class="empty" id="galleryEmpty">还没有照片，点击「拍照」试试～</div>
    <div style="margin-top:14px">
      <button class="act" onclick="clearGallery()">
        <svg viewBox="0 0 24 24"><path d="M6 19a2 2 0 0 0 2 2h8a2 2 0 0 0 2-2V7H6v12zM19 4h-3.5l-1-1h-5l-1 1H5v2h14V4z"/></svg>
        <span>清空相册</span>
      </button>
    </div>
  </div>

  <footer>ESP32-CAM 智能家居 · <span id="fwInfo">WiFi: —</span></footer>
</div>

<div class="toast" id="toast"></div>

<script>
const $ = id => document.getElementById(id);
const video = $('video'), canvas = $('canvas');
let streaming = true, recording = false, recorder = null;
let chunks = [], recTimer = null, recStart = 0;
let shots = [];

/* ---------- toast ---------- */
let toastT = null;
function toast(msg){
  const t = $('toast'); t.textContent = msg; t.classList.add('show');
  clearTimeout(toastT); toastT = setTimeout(()=>t.classList.remove('show'),2200);
}

/* ---------- camera control ---------- */
function ctl(v, val, labelId){
  fetch('/control?var='+v+'&val='+val).catch(()=>{});
  if(labelId) $(labelId).textContent = val;
}
function setRes(v){
  ctl('framesize', v);
  $('vRes').textContent = $('selRes').selectedOptions[0].textContent;
  $('resTag').textContent = $('resTag').dataset.short = 
    $('selRes').selectedOptions[0].textContent.split(' ')[0];
}

/* ---------- video stream ---------- */
let streamTimeout = null;
function startStream(){
  $('loader').classList.remove('hidden');
  clearTimeout(streamTimeout);
  // Cache-bust so a previous failed connection is not reused.
  video.src = '/stream?' + Date.now();
  video.classList.remove('hidden');
  canvas.classList.add('hidden');
  $('stage').classList.remove('off');
  $('placeholder').style.display = 'none';
  // If no frame arrives within 5s, tell the user instead of showing black.
  streamTimeout = setTimeout(()=>{
    if(streaming && !video.naturalWidth){
      $('loader').classList.add('hidden');
      toast('视频流无响应，请检查设备连接');
    }
  }, 5000);
}
function stopStream(){
  clearTimeout(streamTimeout);
  video.removeAttribute('src');
  video.classList.add('hidden');
  $('loader').classList.add('hidden');
  $('stage').classList.add('off');
  $('placeholder').style.display = 'grid';
  drawLastFrame();
}
video.addEventListener('load', ()=>{
  clearTimeout(streamTimeout);
  $('loader').classList.add('hidden');
  netOK(true, $('netText').textContent.replace(' · 视频异常',''));
});
video.addEventListener('error', ()=>{
  if(!streaming) return;
  $('loader').classList.add('hidden');
  toast('视频流连接失败');
});

function toggleVideo(){
  streaming = !streaming;
  const btn = $('btnVideo');
  if(streaming){
    startStream();
    btn.classList.add('primary');
    btn.querySelector('span').textContent = '关闭视频';
    toast('视频已开启');
  }else{
    stopStream();
    btn.classList.remove('primary');
    btn.querySelector('span').textContent = '打开视频';
    toast('视频已关闭');
  }
}

/* keep a frozen frame on the canvas when the stream stops */
function drawLastFrame(){
  if(!video.naturalWidth) return;
  canvas.width = video.naturalWidth; canvas.height = video.naturalHeight;
  canvas.getContext('2d').drawImage(video,0,0);
  canvas.classList.remove('hidden');
}

/* ---------- photo ---------- */
function takePhoto(){
  const c = document.createElement('canvas');
  const w = video.naturalWidth || 640, h = video.naturalHeight || 480;
  c.width = w; c.height = h;
  // Prefer a fresh capture from the device for full quality
  fetch('/capture?' + Date.now())
    .then(r => r.blob())
    .then(blob => {
      const url = URL.createObjectURL(blob);
      addShot(url, true);
      toast('已保存到相册');
    })
    .catch(() => {
      const ctx = c.getContext('2d');
      ctx.drawImage(video,0,0,w,h);
      addShot(c.toDataURL('image/jpeg',0.92), false);
      toast('已保存到相册');
    });
}

function addShot(src, isBlob){
  const item = {src, isBlob, ts:Date.now()};
  shots.unshift(item);
  renderGallery();
  // auto download
  const a = document.createElement('a');
  a.href = src; a.download = 'esp32cam_' + fmtTime(item.ts) + '.jpg';
  document.body.appendChild(a); a.click(); a.remove();
}
function renderGallery(){
  const g = $('gallery');
  g.innerHTML = '';
  $('galleryEmpty').style.display = shots.length ? 'none' : 'block';
  $('shotCount').textContent = shots.length ? '· ' + shots.length + ' 张' : '';
  shots.forEach((s,i)=>{
    const d = document.createElement('div'); d.className = 'shot';
    d.innerHTML = '<img src="'+s.src+'"><button title="删除">×</button>';
    d.querySelector('img').onclick = ()=>{
      const w = window.open(''); w.document.write('<title>照片</title><img src="'+s.src+'" style="max-width:100%">');
    };
    d.querySelector('button').onclick = ()=>{
      if(s.isBlob) URL.revokeObjectURL(s.src);
      shots.splice(i,1); renderGallery();
    };
    g.appendChild(d);
  });
}
function clearGallery(){
  shots.forEach(s=>{ if(s.isBlob) URL.revokeObjectURL(s.src); });
  shots = []; renderGallery(); toast('相册已清空');
}

/* ---------- LED ---------- */
async function toggleLed(){
  try{
    const r = await fetch('/led?state=toggle');
    const j = await r.json();
    const on = j.led === 'on';
    $('btnLed').classList.toggle('on', on);
    toast(on ? '补光灯已打开' : '补光灯已关闭');
  }catch(e){ toast('操作失败'); }
}
async function syncLed(){
  try{
    const r = await fetch('/status'); const j = await r.json();
    $('btnLed').classList.toggle('on', j.led === 1);
  }catch(e){}
}

/* ---------- recording (client-side from stream frames) ---------- */
function toggleRec(){
  if(recording){ stopRec(); return; }
  if(!streaming){ toast('请先打开视频再录制'); return; }
  try{
    // mjpeg <img> cannot be used by MediaRecorder directly; record a canvas
    const w = video.naturalWidth || 640, h = video.naturalHeight || 480;
    canvas.width = w; canvas.height = h;
    const ctx = canvas.getContext('2d');
    const cstream = canvas.captureStream(15);
    const draw = ()=>{
      if(!recording) return;
      if(video.naturalWidth) ctx.drawImage(video,0,0,w,h);
      requestAnimationFrame(draw);
    };
    const types = ['video/webm;codecs=vp9','video/webm;codecs=vp8','video/webm'];
    const mime = types.find(t=>MediaRecorder.isTypeSupported(t)) || '';
    recorder = new MediaRecorder(cstream, mime?{mimeType:mime}:undefined);
    chunks = [];
    recorder.ondataavailable = e => { if(e.data.size) chunks.push(e.data); };
    recorder.onstop = ()=>{
      const blob = new Blob(chunks, {type: recorder.mimeType || 'video/webm'});
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url; a.download = 'esp32cam_' + fmtTime(Date.now()) + '.webm';
      document.body.appendChild(a); a.click(); a.remove();
      setTimeout(()=>URL.revokeObjectURL(url), 4000);
      toast('录制已保存');
    };
    recorder.start(1000);
    recording = true; recStart = Date.now(); draw();
    $('btnRec').classList.add('on');
    $('btnRec').querySelector('span').textContent = '停止录制';
    $('recTag').classList.remove('hidden');
    updateRecTimer();
    toast('开始录制…');
  }catch(e){ toast('此浏览器不支持录制'); }
}
function stopRec(){
  recording = false;
  if(recorder && recorder.state !== 'inactive') recorder.stop();
  clearInterval(recTimer);
  $('btnRec').classList.remove('on');
  $('btnRec').querySelector('span').textContent = '开始录制';
  $('recTag').classList.add('hidden');
}
function updateRecTimer(){
  clearInterval(recTimer);
  recTimer = setInterval(()=>{
    if(!recording) return;
    const s = Math.floor((Date.now()-recStart)/1000);
    $('recTag').textContent = '录制中 ' + String(Math.floor(s/60)).padStart(2,'0') + ':' + String(s%60).padStart(2,'0');
  },500);
}

/* ---------- fullscreen ---------- */
function fullscreen(){
  const el = $('stage');
  if(document.fullscreenElement) document.exitFullscreen();
  else if(el.requestFullscreen) el.requestFullscreen();
  else if(video.webkitEnterFullscreen) video.webkitEnterFullscreen();
}

/* ---------- misc ---------- */
function fmtTime(ts){
  const d = new Date(ts), p = n => String(n).padStart(2,'0');
  return d.getFullYear()+p(d.getMonth()+1)+p(d.getDate())+'_'+p(d.getHours())+p(d.getMinutes())+p(d.getSeconds());
}
function netOK(ok, text){
  $('netDot').classList.toggle('on', ok);
  $('netText').textContent = text;
}

/* ---------- init ---------- */
async function loadStatus(){
  try{
    const r = await fetch('/status'); const j = await r.json();
    netOK(true, 'WiFi ' + j.rssi + ' dBm');
    $('fwInfo').textContent = 'WiFi: ' + j.rssi + ' dBm';
    $('btnLed').classList.toggle('on', j.led === 1);
    // reflect settings
    $('rQual').value = j.quality; $('vQual').textContent = j.quality;
    $('selRes').value = j.framesize;
    $('vRes').textContent = $('selRes').selectedOptions[0].textContent;
    $('resTag').textContent = $('selRes').selectedOptions[0].textContent.split(' ')[0];
    $('rBri').value = j.brightness; $('vBri').textContent = j.brightness;
    $('rCon').value = j.contrast;   $('vCon').textContent = j.contrast;
    $('rSat').value = j.saturation; $('vSat').textContent = j.saturation;
    $('rAe').value = j.ae_level;    $('vAe').textContent = j.ae_level;
    $('swH').checked = !!j.hmirror; $('swV').checked = !!j.vflip;
    $('swWb').checked = !!j.whitebal; $('swAe').checked = !!j.exposure_ctrl;
    $('swAg').checked = !!j.gain_ctrl;
    $('selWb').value = j.wb_mode; $('selFx').value = j.special_effect;
  }catch(e){ netOK(false, '设备离线'); }
}

window.addEventListener('load', ()=>{
  startStream();
  loadStatus();
  setInterval(loadStatus, 8000);
  document.addEventListener('visibilitychange', ()=>{
    if(!document.hidden && streaming) startStream();
  });
});
</script>
</body>
</html>
)HTMLPAGE";

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

// =========================================================
//  PHONE APP LINK  -  UDP discovery + TCP client
// =========================================================
// The ESP32-CAM is the TCP *client*.  It cannot know the phone app's IP
// up-front (it is dynamic / not provided), so it finds the app on the LAN
// by itself, connects, and reports its own web URL (http://<esp-ip>/) so
// the app can open the live stream.
//
// Discovery : ESP32 broadcasts a UDP probe; the app answers with its TCP
//             port.  If the app never answers UDP, ESP32 can fall back to
//             scanning its own /24 subnet for the TCP server.
// Transport : line based JSON, '\n' terminated.
//
//   ESP32 -> APP   UDP broadcast :4210
//       SMARTHOUSE_DISCOVER:ESP32CAM\n
//   APP   -> ESP32 UDP unicast (same port)
//       SMARTHOUSE_SERVER:4211\n
//   ESP32 -> APP   TCP :4211
//       {"type":"register","name":"ESP32-CAM","ip":"...","port":80,
//        "url":"http://.../","rssi":-52}\n
//       {"type":"heartbeat",...}\n          every APP_HEARTBEAT_MS
// ---------------------------------------------------------
#define APP_DISCOVERY_PORT   4210    // UDP discovery port (both sides)
#define APP_TCP_PORT         4211    // default TCP port of the phone app
#define APP_DISCOVERY_MS     3000    // send a probe every 3 s
#define APP_HEARTBEAT_MS     10000   // keep-alive every 10 s
#define APP_CONNECT_TIMEOUT  1500    // ms
#define APP_RETRY_MS         3000    // reconnect attempt spacing
#define APP_RETRY_LIMIT      3       // failed connects before re-discover
#define APP_SCAN_FALLBACK    1       // 1 = scan /24 if UDP gets no answer
#define APP_SCAN_START_MS    15000   // start scanning after this long
#define APP_SCAN_HOST_MS     120     // one host probed per interval
#define APP_SCAN_TIMEOUT     200     // ms per host during a scan

static WiFiUDP    appUdp;
static WiFiClient appTcp;

static bool       appUdpStarted   = false;
static bool       appServerKnown  = false;
static IPAddress  appServerIP;
static uint16_t   appServerPort   = APP_TCP_PORT;
static uint32_t   appLastProbe    = 0;
static uint32_t   appLastConnect  = 0;
static uint32_t   appLastBeat     = 0;
static uint32_t   appDiscoverRef  = 0;   // when discovery started (scan guard)
static uint32_t   appLastScan     = 0;
static int        appScanHost     = 1;
static int        appConnFails    = 0;

// Subnet-directed broadcast address, e.g. 192.168.1.255
static IPAddress appBroadcastIP() {
  IPAddress ip   = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  return IPAddress((uint8_t)(ip[0] | ~mask[0]),
                   (uint8_t)(ip[1] | ~mask[1]),
                   (uint8_t)(ip[2] | ~mask[2]),
                   (uint8_t)(ip[3] | ~mask[3]));
}

static String appUrl() {
  return String("http://") + WiFi.localIP().toString() + "/";
}

// ---- UDP discovery -------------------------------------------------
static void appSendProbe() {
  const uint8_t *p   = (const uint8_t *)"SMARTHOUSE_DISCOVER:ESP32CAM\n";
  size_t         len = strlen((const char *)p);

  IPAddress bcast = appBroadcastIP();
  appUdp.beginPacket(bcast, APP_DISCOVERY_PORT);
  appUdp.write(p, len);
  appUdp.endPacket();

  // Global broadcast as well: survives unusual subnet masks.
  appUdp.beginPacket(IPAddress(255, 255, 255, 255), APP_DISCOVERY_PORT);
  appUdp.write(p, len);
  appUdp.endPacket();

  Serial.printf("[APP] probe -> %s:%u\n", bcast.toString().c_str(),
                APP_DISCOVERY_PORT);
}

static void appPollReply() {
  int sz = appUdp.parsePacket();
  if (sz <= 0) return;

  IPAddress from = appUdp.remoteIP();
  char buf[96];
  int  n = appUdp.read(buf, sizeof(buf) - 1);
  if (n <= 0) return;
  buf[n] = '\0';

  if (strncmp(buf, "SMARTHOUSE_SERVER", 17) != 0) return;

  uint16_t port = APP_TCP_PORT;
  char *colon = strrchr(buf, ':');
  if (colon != NULL && atoi(colon + 1) > 0) port = (uint16_t)atoi(colon + 1);

  if (!appServerKnown || appServerIP != from || appServerPort != port) {
    Serial.printf("[APP] server found at %s:%u\n", from.toString().c_str(),
                  port);
  }
  appServerIP    = from;
  appServerPort  = port;
  appServerKnown = true;
  appConnFails   = 0;
}

// ---- TCP client ----------------------------------------------------
static void appSendRegister() {
  char msg[192];
  snprintf(msg, sizeof(msg),
           "{\"type\":\"register\",\"name\":\"ESP32-CAM\","
           "\"ip\":\"%s\",\"port\":80,\"url\":\"%s\",\"rssi\":%d}\n",
           WiFi.localIP().toString().c_str(), appUrl().c_str(),
           (int)WiFi.RSSI());
  appTcp.print(msg);
  Serial.printf("[APP] >> %s", msg);
}

static size_t appSendHeartbeat() {
  char msg[192];
  snprintf(msg, sizeof(msg),
           "{\"type\":\"heartbeat\",\"ip\":\"%s\",\"port\":80,"
           "\"url\":\"%s\",\"rssi\":%d}\n",
           WiFi.localIP().toString().c_str(), appUrl().c_str(),
           (int)WiFi.RSSI());
  return appTcp.print(msg);
}

static bool appConnect() {
  if (!appServerKnown) return false;

  appTcp.stop();
  Serial.printf("[APP] TCP connect %s:%u ...\n",
                appServerIP.toString().c_str(), appServerPort);
  if (!appTcp.connect(appServerIP, appServerPort, APP_CONNECT_TIMEOUT)) {
    Serial.println("[APP] connect failed");
    return false;
  }
  Serial.println("[APP] connected");
  appTcp.setTimeout(200);   // keep readStringUntil() from blocking long
  appSendRegister();
  appLastBeat = millis();
  return true;
}

static void appReadFromServer() {
  while (appTcp.available()) {
    String line = appTcp.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    Serial.printf("[APP] << %s\n", line.c_str());
    // Example command handling:
    // if (line == "ping") appTcp.print("{\"type\":\"pong\"}\n");
  }
}

// Fallback: probe one host of the local /24 for the app's TCP port.
static void appScanStep() {
  IPAddress local = WiFi.localIP();
  if (appScanHost > 254) appScanHost = 1;
  uint8_t host = (uint8_t)appScanHost++;

  IPAddress target(local[0], local[1], local[2], host);
  if ((uint32_t)target == (uint32_t)local) return;

  WiFiClient probe;
  if (probe.connect(target, APP_TCP_PORT, APP_SCAN_TIMEOUT)) {
    Serial.printf("[APP] TCP server found by scan at %s:%u\n",
                  target.toString().c_str(), APP_TCP_PORT);
    appServerIP    = target;
    appServerPort  = APP_TCP_PORT;
    appServerKnown = true;
    appConnFails   = 0;
  }
  probe.stop();
}

static void appClientBegin() {
  if (appUdpStarted) return;
  appUdpStarted = (appUdp.begin(APP_DISCOVERY_PORT) == 1);
  Serial.printf("[APP] UDP discovery %s (port %u)\n",
                appUdpStarted ? "ready" : "FAILED", APP_DISCOVERY_PORT);
}

// Call this from loop() - fully non blocking.
static void appClientLoop() {
  uint32_t now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (appTcp.connected()) appTcp.stop();
    appServerKnown  = false;
    appDiscoverRef  = now;
    appScanHost     = 1;
    return;
  }

  appClientBegin();
  appPollReply();   // drain discovery replies at all times

  // -------- connected: heartbeat + read --------
  if (appTcp.connected()) {
    appReadFromServer();
    if (now - appLastBeat >= APP_HEARTBEAT_MS) {
      appLastBeat = now;
      if (appSendHeartbeat() == 0) {   // write failed -> link is gone
        Serial.println("[APP] heartbeat failed - disconnected");
        appTcp.stop();
        return;
      }
    }
    return;
  }

  // -------- known server: (re)connect --------
  if (appServerKnown) {
    if (now - appLastConnect >= APP_RETRY_MS) {
      appLastConnect = now;
      if (appConnect()) {
        appConnFails = 0;
      } else if (++appConnFails >= APP_RETRY_LIMIT) {
        Serial.println("[APP] too many failures - re-discovering");
        appServerKnown = false;
        appConnFails   = 0;
        appDiscoverRef = now;
        appScanHost    = 1;
      }
    }
    return;
  }

  // -------- unknown server: discover --------
  if (now - appLastProbe >= APP_DISCOVERY_MS) {
    appLastProbe = now;
    appSendProbe();
  }

#if APP_SCAN_FALLBACK
  if (now - appDiscoverRef >= APP_SCAN_START_MS &&
      now - appLastScan >= APP_SCAN_HOST_MS) {
    appLastScan = now;
    appScanStep();
  }
#endif
}
// =========================================================

// ---------------------------------------------------------
//  Start the HTTP servers
// ---------------------------------------------------------
static void startCameraServer() {
  // Single server on port 80 serves the UI, control endpoints and the
  // MJPEG stream, so all relative URLs in the page (/stream, /capture,
  // /status, /control, /led) resolve correctly.
  httpd_config_t cam_cfg = HTTPD_DEFAULT_CONFIG();
  cam_cfg.server_port      = 80;
  cam_cfg.ctrl_port        = 32769;
  cam_cfg.max_open_sockets = 7;      // ESP32 max; streaming needs headroom
  cam_cfg.lru_purge_enable = true;   // reclaim stale sockets
  cam_cfg.recv_wait_timeout = 10;
  cam_cfg.send_wait_timeout = 10;

  httpd_uri_t index_uri    = {"/",        HTTP_GET, index_handler,    NULL, false, false, NULL};
  httpd_uri_t capture_uri  = {"/capture", HTTP_GET, capture_handler,  NULL, false, false, NULL};
  httpd_uri_t led_uri      = {"/led",     HTTP_GET, led_handler,      NULL, false, false, NULL};
  httpd_uri_t control_uri  = {"/control", HTTP_GET, control_handler,  NULL, false, false, NULL};
  httpd_uri_t status_uri   = {"/status",  HTTP_GET, status_handler,   NULL, false, false, NULL};
  httpd_uri_t stream_uri   = {"/stream",  HTTP_GET, stream_handler,   NULL, false, false, NULL};

  if (httpd_start(&camera_httpd, &cam_cfg) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &led_uri);
    httpd_register_uri_handler(camera_httpd, &control_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    httpd_register_uri_handler(camera_httpd, &stream_uri);
    Serial.println("[HTTP] camera+stream server on port 80");
  } else {
    Serial.println("[HTTP] failed to start server");
  }
}

// ---------------------------------------------------------
//  Connect to WiFi (STA)
// ---------------------------------------------------------
static bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.printf("[WiFi] connecting to \"%s\"", WIFI_SSID);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - start > WIFI_TIMEOUT_MS) {
      Serial.println("\n[WiFi] timeout - continuing without network");
      return false;
    }
  }
  Serial.println();
  Serial.print("[WiFi] connected. IP: ");
  Serial.println(WiFi.localIP());
  Serial.printf("[WiFi] RSSI: %d dBm\n", WiFi.RSSI());
  return true;
}

// ---------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  delay(200);

  Serial.println();
  Serial.println("=== SmartHouse ESP32-CAM ===");

  pinMode(FLASH_LED_GPIO, OUTPUT);
  digitalWrite(FLASH_LED_GPIO, LOW);

  if (!initCamera()) {
    Serial.println("[FATAL] camera init failed - halting");
    while (true) delay(1000);
  }

  bool wifiOK = connectWiFi();

  // Start the web server and the phone-app link regardless of the initial
  // WiFi result.  appClientLoop() keeps retrying, and the web UI becomes
  // reachable as soon as the network is up.
  startCameraServer();
  appClientBegin();

  if (wifiOK) {
    Serial.println("-------------------------------------------");
    Serial.printf("Web UI       :    http://%s/\n", WiFi.localIP().toString().c_str());
    Serial.printf("Live stream  :    http://%s/stream\n", WiFi.localIP().toString().c_str());
    Serial.printf("Snapshot     :    http://%s/capture\n", WiFi.localIP().toString().c_str());
    Serial.printf("Status       :    http://%s/status\n", WiFi.localIP().toString().c_str());
    Serial.printf("Phone app    :    discovering UDP :%u ...\n", APP_DISCOVERY_PORT);
    Serial.println("-------------------------------------------");
  } else {
    Serial.println("[WARN] no WiFi - camera running but not accessible");
  }
}

void loop() {
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck > 5000) {
    lastCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] disconnected - reconnecting...");
      WiFi.reconnect();
    }
  }

  appClientLoop();   // UDP discovery + TCP client (non blocking)
  delay(10);
}
