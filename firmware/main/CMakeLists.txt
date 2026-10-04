# The dashboard is baked into the app binary. Run `npm run build` in ../frontend first.
set(www ../../frontend/dist)
idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES esp_wifi nvs_flash esp_http_server esp_driver_tsens esp_timer json
                       EMBED_FILES ${www}/index.html.gz ${www}/assets/app.js.gz ${www}/assets/app.css.gz)
