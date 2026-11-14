SDK example: a process plugin in plain shell. See docs/plugin-sdk.md.
  chmod +x examples/plugins/hello-process/bin/run.sh   (a web upload can drop the executable bit)
  pnr-launcher plugin add examples/plugins/hello-process
  pnr-launcher plugin enable example.hello-process --grant library_read,fs_write_plugin_data
  pnr-launcher plugin run example.hello-process
It runs only inside the partial sandbox and can use only what you granted.
