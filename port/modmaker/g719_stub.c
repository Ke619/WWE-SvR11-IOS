/* libg719_decode.dll for Mod Maker Tools\vgmstream: vgmstream-cli.exe won't start without a DLL of this name, and
   none of the game audio the converters feed it uses G.719. This one decodes nothing: g719_init fails, so vgmstream
   refuses a G.719 file (Namco BNSF, codec IS22) instead of opening it. Exports as the original's, by ordinal
   (g719_stub.def). */
__declspec(dllexport) void* g719_init(int frame_size) {
  (void)frame_size;
  return 0;
}
__declspec(dllexport) void g719_free(void* handle) { (void)handle; }
__declspec(dllexport) void g719_reset(void* handle) { (void)handle; }
__declspec(dllexport) void g719_decode_frame(void* handle, void* code, void* samples) {
  (void)handle, (void)code, (void)samples;
}
