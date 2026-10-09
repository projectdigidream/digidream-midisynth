# DigidreamSynth web tools -- static bundle behind nginx.
#
#   ./scripts/build_wasm.sh && ./scripts/package_web.sh
#   docker build -f deploy/Dockerfile -t digidreamsynth .
#   docker run -p 8080:80 digidreamsynth
#
# Terminate TLS in front of it (AudioWorklet/Web MIDI need HTTPS).
FROM nginx:alpine
COPY dist-web/ /usr/share/nginx/html/
RUN printf 'types { text/javascript mjs; audio/midi mid; }\n' \
      > /etc/nginx/conf.d/digidreamsynth-types.conf
