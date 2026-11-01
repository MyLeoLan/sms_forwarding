idf_component_register(
  SRCS "idf_imb.cpp"
  INCLUDE_DIRS "include"
  REQUIRES
    esp_http_client
    esp_timer
    esp-tls
    idf_config
    idf_logbuf
    idf_modem
    idf_sms
    idf_wifi
    mbedtls
)
