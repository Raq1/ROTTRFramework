// (v0.9.543) The ray tracing probe (see rt_probe.h). Retail RVAs of the Steam ROTTR.exe, from TR2_x64_final's functions
// of the same names, each byte-checked.
//
// Where each piece comes from:
// - The draws: cdc::PCDX12RenderContext::DrawIndexedInstanced (TR2 0x14032B530, retail 0x308330) calls PreDraw (the
//   pipeline, vertex / index buffers, constants bound) and then the D3D12 command list at context +0x3488
//   (DrawIndexedInstanced, its vtable slot 13). It is hooked to mark the thread's current context for the D3D12 call
//   inside it; the D3D12 command lists' DrawIndexedInstanced, IASetVertexBuffers (44), IASetIndexBuffer (43) and
//   IASetPrimitiveTopology (20) are hooked in their vtable (eight_weight_patch.cpp's CreateCommandList hook hands each
//   list over), so a draw inside the engine's call is seen with the buffers bound for it. Other D3D12 calls (the
//   overlay's own, ImGui) have no context and are left alone.
// - The pipeline: eight_weight_patch.cpp keeps every game pipeline's description (its wireframe record);
//   EightWeightPatch::PipelineGeometryOf reads its input layout (POSITION: format, slot, offset), whether it is skinned
//   (BLENDINDICES / BLENDWEIGHT), writes depth, and draws triangles. OnSetPipelineState (from the SetPipelineState /
//   Reset hooks, with the game's own pipeline) keeps it for the list.
// - The transform: cdc::RenderContext's world matrix at +0xE50 (RenderContext::SetWorldMatrix, TR2 0x1402A0E90, retail
//   0x2827F0, writes it there and sets dirty bit 0x40 at +0xF54; PreDraw uploads it): rows = axes, row 3 = position
//   (v x M); DXR wants its transpose's top three rows.
// - The camera: Features' draw camera (the rendered camera: position, rotation, vertical field of view), snapshotted on
//   the game thread while on.
// A draw's mesh: positions from the vertex buffer view at the POSITION slot (+ its offset + base vertex x stride), as
// many vertices as the view holds from there; indices from the index buffer view (+ start index). Its key hashes those.
// Not taken (counted): skinned meshes (Lara, the characters: skinned in their vertex shaders, so their bind pose is all
// a buffer holds), instanced draws, other position formats, other topologies, draws without depth writes (particles,
// decals, the transparent ones), non-indexed draws.
// (v0.9.544) The user's first run (screenshots: the main menu's study, a forest, the Soviet Installation; the log):
// shapes and places right; all positions R32G32B32_FLOAT (DXGI 6); per frame in play ~950 skinned and ~560 instanced
// draws not taken (the forest's trees gone), ~35 tessellated (topology: ground missing under objects), the sky's
// geometry taken (a pink cylinder around the scene, grey cards in the sky); 12,065 meshes built before the 1.5 GB budget,
// then 18,785 refused (objects missing). Now:
// - instanced draws: cdc::ModelInstancingDrawable::Draw (TR2 0x1402C3D60, retail 0x2A4A60; SetInstanceCount with its
//   +0x20, SetWorldMatrix with the first instance's) is hooked: its instances (+0x18: a list of records, +0x20 their
//   count; a record's +0x28 -> its world matrix at +0x50, rows as SetWorldMatrix's) are read at its start, and an
//   instanced draw inside it with that many instances gives one object per instance (its own matrix). Grass
//   (GrassInstancingDrawable) is not that drawable and stays out.
// - the sky: cdc::RenderContext::SetSkySphereMode (TR2 0x1402A0210, retail 0x281B70) stores the mode at +0xD41, in
//   effect while +0xD40 (SetSkySphereModeEnabled) is set; draws made in it are left out.
// - tessellated draws (a patch pipeline, 3 control points a patch): their control points as triangles (the surface
//   before the domain shader moves it).
// - the meshes' memory: the oldest let go when the budget is reached (rt_probe_gpu.cpp), rebuilt when drawn again.
// (v0.9.545) Skinned meshes (Lara, the characters): posed on the GPU each frame as the game's skinned vertex shaders pose
// them (rt_probe_skin.hlsl). cdc::ModelDrawable::SetupSkinningMatrices(this, RenderContext*, IConstantBuffer* cb,
// IConstantBuffer* previous, int instance) (TR2 0x1402E88D0, retail 0x2C5A60; from ModelDrawable::SetupMatrixState
// before each skinned draw, and ModelInstancingDrawable::Draw) writes the draw's palette into the cb's data ([cb+8]):
// +0 the count (a float), from +0x10 64 bytes an entry (the bone's world skinning matrix as its three columns, then the
// bone's effects row), entry i = the model's matrix boneList[i]; the shader (SkinningBuffer, cb1) poses a vertex as
// sum over its four (BLENDWEIGHT, BLENDINDICES x 255) of weight x dot(column, (position, 1)): absolute world space.
// Hooked: after the game's, the palette is copied for the thread (1,023 entries at most) and hashed; a skinned draw then
// gives its streams (position, BLENDWEIGHT 0, BLENDINDICES 0 from the pipeline's layout and the bound views), its index
// range and that palette; the same mesh with the same palette (its other passes: depth, normals, shadows) once a frame.
// Not taken: the 8-weight layout (a second weight / index set: counted), weight / index formats the shader does not
// read, skinned draws with no palette on the thread (counted).
// (v0.9.546) The user's run: the characters posed and in place, but long thin triangles from them across the scene,
// some between Lara and the people beside her: parts posed with another model's palette. The thread's last palette is
// not always the draw's (a draw recorded on another thread than its SetupMatrixState, or its matrix state made earlier).
// Now the palette goes with the constant buffer it was written into: SetupSkinningMatrices' copy is kept by its cb, and
// a skinned draw takes the one of the cb bound as the vertex shader's b1 (SkinningBuffer). ModelDrawable::
// SetupMatrixState binds it with the context's SetConstantBuffer(stage 0 vertex, slot 1, cb) (vfunc +0x78; retail
// PCDX12RenderContext::SetConstantBuffer 0x313870, TR2 0x140336AC0), which keeps it at context
// +((stage + 15) x 80 + slot) x 8: +0x2588.
// (v0.9.547) The user's run: no triangles between characters any more, but two on Lara herself (a fan from the shoulder
// past the hip, a wedge from the hand): one of her parts posed wrong. The game's skinned pipelines (a vertex layout with
// BLENDWEIGHT / BLENDINDICES) do not all skin. The skinned vertex shaders of the game's libraries (pcx64-w
// sofia_cine.drm), by what they read: about 75 variants four weights and four indices; about 65 the first index alone
// (one bone: palette entry round(BLENDINDICES.x x 255), the pose its matrix x (position, 1)); about 34 neither (the
// object's WorldBuffer matrix: drawn as a static mesh; one adds the face's blend shape offsets, t49). Their input
// signature's read masks (EightWeightPatch::PipelineGeometry.weightUse / blendIndexUse) now decide: indices read: posed
// with the components read (no weights read: one bone); none read: taken as a static mesh (the context's world
// matrix); no signature: left out.
// (v0.9.548) The user's next run: long thin triangles again, many, some seeming to meet at one point (a forest by a tomb's
// entrance; a hunters' platform, from a man standing there). Not known which capture they come from: each object's
// instance ID now carries its kind in bits 21..23 (0 static, 1 instanced, 2 a skinned layout drawn with the world
// matrix, 3 four-weight skinned, 4 one-bone skinned, 5 tessellated); "Colour by kind" colours the view by it, and the
// 5 s log line counts a frame's objects of each kind.
// (v0.9.549) "Colour by kind" in the game: the long triangles red and blue (four-weight and one-bone skinned), many from one
// character to another. A skinned mesh's vertex count was taken from its streams with 16 bytes for the weight and index
// elements: one or two vertices short for 4-byte elements, and the triangles using the last vertices read past the
// mesh's slice of the posed buffer into the next mesh's. Now each element at its format's size.
// (v0.9.550) The user's next run (a camp): the triangles gone, Lara whole, but parts of the people missing (a standing man
// without his torso and arms, his legs and head there). About 1,100 skinned draws a frame, 195 taken and 311 more passes of
// them: the rest not taken, their reason not counted apart. One left out on purpose: instanced skinned draws.
// cdc::ModelInstancingDrawable::Draw (TR2 0x1402C3D60, retail 0x2A4A60) draws the instances of one model in one draw: a SkinningBuffer made
// for count x instances entries, SetupSkinningMatrices for each instance i with the same cb and i (writes instance i's
// palette at +0x10 + i x count x 64, the count at +0), then DrawIndexedInstanced. Its vertex shaders (sofia_cine.drm
// 57052..57054) take bone entry floor(SV_InstanceID x NumSkinMatricesPerInstance) + round(BLENDINDICES x 255): instance
// i's palette. People in the same clothes (a shared torso, their own legs and heads) are drawn so.
// Now: SetupSkinningMatrices' copy keeps each instance's palette (in turn after instance 0; one out of turn makes the cb's
// unusable until its next instance 0), and an instanced skinned draw gives one skinned mesh an instance, posed with its
// own palette (kind 6, orange). The skinned draws not taken are counted by reason; a skinned draw without depth writes
// (another pass of a mesh taken: the colour pass after the depth one) is kept apart and taken at the frame's end only
// when no pass that writes depth gave the same mesh and palette (kind 7, pink): a part drawn only so.
// (v0.9.551) The user's run: the people whole (their shared clothes orange; the pots, bushes and branches too: the game's
// instanced skinned models). But about 700 skinned meshes a frame now: the GPU side's limit (512 meshes, 6 M vertices)
// reached, 170 to 200 a frame dropped in the order the draws came (the recording threads'): which ones, by chance.
// Now the frame's skinned meshes go to the GPU side nearest the camera first (the distance from the camera to the
// palette's first entry's translation: the model's origin in the world), and the limits are 1,024 meshes and 8 M
// vertices; the log gives the farthest one posed.
// (v0.9.552) Leaf cutouts. The user's run: the skinned meshes all there (the camp: about 900 a frame, none over the
// limits; a forest: the 1,024 nearest, out to 11,000..13,000 units). The foliage is solid cards: the game's alpha tests
// were not followed. Now (rt_cutout.h) each pipeline's alpha test is analyzed as it is made (the pixel shader's first
// discard back to one texture's component at a TEXCOORD x the material's tiling, against DrawableBuffer.AlphaThreshold;
// the vertex shader's TEXCOORD output back to its input x StreamDeclBuffer.TexcoordScales). Offline, over sofia_cine's
// libraries: 107 of the 136 discarding pixel shaders followed (13 more are screen dithers: no texture; 16 move their UVs),
// every vertex shader's TEXCOORD 0. A draw with a followed test resolves it from the engine's context (retail
// PCDX12RenderContext::SetShaderResourceView 0x314BE0, TR2 0x140337E30):
// - its constant buffers: as SetConstantBuffer keeps them, +0x2580 + (stage x 80 + slot) x 8 (stage: 0 vertex, 4 pixel:
//   the sampler setters' dirty bits, vertex hull domain geometry pixel compute); a buffer's size at +0x14, its data at
//   +0x08 (a static one: its own copy; a dynamic one: null once unmapped, then its pool buffer +0x48 at +0x38);
// - its texture: the descriptor cache [+0x3490]'s CPU descriptors, +0x348 + (stage x 0xAD + slot) x 8 (the null one:
//   [[+0x3490]+0x2220]+0x328, not taken).
// The mesh is then built not opaque, its UVs gathered once, and its rays test the texel (rt_probe_gpu.cpp,
// rt_probe.hlsl).
// (v0.9.553) The user's run: Lara's hair cards cut out (strands), the foliage still solid. Per frame 2 draws with their
// test resolved, 200 to 700 not (texture / constants), the step not known. The failures are now counted by step, and the
// first few of each logged with what was read (the UV element and view, the texture's descriptor, each constant's
// buffer, size, data or pool and offset); the first resolved ones with their values.
// (v0.9.554) The user's run (a forest): per frame 430..810 stopped at the threshold, 20..50 at a layout without the
// TEXCOORD. DrawableBuffer is a dynamic buffer: its data pointer null once unmapped, and its resource (+0x48) not CPU
// visible (Map: E_INVALIDARG). cdc::PCDX12DynamicPool (RenderDevice +0x2B4B8 retail, +0x2B578 TR2) keeps a block's CPU
// copy apart: Allocate (TR2 0x140323480, retail 0x300570) returns the block's +0x10 (CPU) + the offset and gives its +0x08
// (resource); CopyToGPU copies the blocks over at flush. Its blocks are in lists a frame slot (FindFreeBlock, retail
// 0x309B00): pool +0x10 + slot x 0x30 and +0x18 + slot x 0x30, a block's next at +0x30, its size at +0x20; the slots
// DeviceManager's +0x820. A dynamic buffer's data is now its resource's block's CPU copy at its offset (the retail
// PCDX12DynamicConstantBuffer::Map's bytes checked, 0x30FB80: the two globals and the pool's offset in them). The
// layout of a followed test without its TEXCOORD is logged.
// (v0.9.555) Groundwork for the lamps' ray-traced shadows, and the probe's cost (the user: the frame rate drops
// noticeably with it on). The time is measured: the GPU's per phase (timestamps in the probe's own work: static builds,
// skinned posing and builds, UV gathers, the top level, the view), the CPU's in the draw hooks (all threads) and in the
// frame's work at the present. The lamps: the tiled lights' pass (TiledLightingApplyDrawable::Draw -> CommonRenderDevice::
// DrawQuad) draws with PCDX12RenderContext::DrawInstanced (retail 0x3083A0, TR2 0x14032B5A0; the indexed one's twin) and
// a pipeline of the lights' pass (EightWeightPatch::IsLightPassPipeline). That draw's context is now kept as the indexed
// one's is, and at the first such draw of a frame its lights (STiledLightHotData: pixel cb12, 12 registers a light) and
// SceneBuffer (pixel cb2: View at +0) are copied; the 5 s log gives their count and the first ones (type, position in
// view space and in the world both ways, colour, every register) with the camera from View both ways beside the probe's.
// (v0.9.556) The user's run: the draw hooks took 27 to 93 ms of CPU a frame (all threads), the present 2.3 to 8.9 ms,
// the GPU 10.5 to 27.7 ms (the view 7 to 22, the skinned 2 to 8.5). The hooks' cost cut:
// - the skinned palettes are read in place at the draw (the SkinningBuffer bound as b1: its static copy, or the dynamic
//   pool's CPU copy of it), keyed by their entry count and first eight entries, and copied only for the meshes taken;
//   SetupSkinningMatrices' copy of every palette (hashed whole) and the draw's second copy are gone (kept only when the
//   pool is not there);
// - a pipeline's geometry is made once when the pipeline is made (it was made again at every SetPipelineState: its
//   layout walked, its vertex shader's signature read);
// - the dynamic pool's blocks found are kept per thread (eight), so a lookup rarely walks the lists;
// - the position formats are counted in an array of atomics, not a map behind a mutex at every draw.
// SetPipelineState's part is now timed with the draw hooks.
// The lights (0.9.555's run): View by rows (the camera from it matches the probe's); a light's +0.xyz view position,
// +0.w type (0 omni), +1 colour, +2.x 1 / radius.
// (v0.9.557) The user's run: 200 to 345 ms of CPU a frame in the hooks, now with SetPipelineState's part counted. That
// part asked each pipeline for its record (ID3D12Object::GetPrivateData, locked inside the runtime) at every bind on
// every thread. What is read of a pipeline (its geometry, whether it is the lights' pass's) is now kept per thread
// (64 pipelines), good while no game pipeline has gone since (EightWeightPatch::PipelineGeneration); and the hooks' time
// is logged per part (SetPipelineState, the draws, of them the skinned capture and the cutouts, the instancing reads,
// the palette hook).
// (v0.9.558) The user's run: SetPipelineState down to about 1.3 ms a frame; the draws 150 to 390 ms (the skinned capture
// 100 to 200 of it). Every captured draw, instance and palette took one global mutex, its vectors growing under it: the
// render threads queued on each other. Now each recording thread captures into its own buffer (its own lock, met only by
// the present's swap), and the present merges them: the skinned meshes deduplicated across threads there (each thread
// still keeps a mesh and pose once), the meshes drawn only without depth writes added after. The hash reads words, not
// bytes; the dynamic pool's blocks are kept per thread in 32 entries, and the pool's lookups that missed them (and the
// blocks walked) are counted in the log.
// (v0.9.559) The user's run: the static capture down to about 1.5 ms a frame, the skinned 55 to 100 ms (60 to 100
// microseconds a draw). Its palettes were read from the dynamic pool's CPU copy: an upload heap's mapping,
// write-combined, very slow for the CPU to read; copied whole and their first eight entries hashed. Now a dynamic
// SkinningBuffer's palettes are not read on the CPU but posed from where the GPU has them: the buffer's resource (+0x48)
// GetGPUVirtualAddress + its offset (+0x38), as the engine's own Map makes its view (PCDX12ConstantBuffer::CreateHandle),
// + 16 + instance x entries x 64 (the posing's root view). Only each palette's first entry (64 bytes) is read on the CPU:
// the key (the entries and that entry: the root places the pose) and the distance (its translation). A static buffer
// (its own CPU copy) is copied as before.
// (v0.9.560) The user's run: the hooks 4 to 10 ms of CPU a frame, the characters right. The lamp shadows' first step: a
// mask made in the probe's own work (rt_probe_shadow.hlsl) and shown over the view, to see it line up before the
// lights' pass uses it. Its frame: SceneBuffer (pixel cb2 of the lights' pass, now copied up to register 86): View (rows),
// [18] the projection's scale and offset (view x / z = uv.x x [18].x + [18].z, y / z = uv.y x [18].y + [18].w), [45]
// ScreenExtents (uv = pixel x zw + xy), [46].xy ScreenResolution (the render size; the mask half of it). Its lamps: the
// lights' pass's (cb12): types below 4 (not box lights or the sun), a reach (1 / +2.x) of 5,000 units at most, a colour;
// the three whose reach comes nearest the camera (and within 2,500 units of it), placed in the world by View. A texel's
// ray: the render pixel at its centre mapped as the lights' pass maps it; from its hit, a shadow ray to each lamp in
// reach (stopped 25 units short of it: its own fixture). The log gives the lamps, the mapping's registers and the mask's
// GPU time.
// (v0.9.561) The lamp shadows in the game's lighting. The user's run of 0.9.560: the mask lined up (the rocks' and Lara's
// shadows at their feet, the grass's cut shapes), 1.4 to 2.8 ms of GPU at 1,280 x 720; the lamps taken included fill
// lights of 30 to 43 m (their shadows tinting whole regions): now a reach of 2,000 units (20 m) at most.
// - The mask is made before the lights' pass reads it: ID3D12CommandQueue::ExecuteCommandLists of the game's queue (the
//   overlay's) is hooked (its vtable); the first submission after a frame's lights were copied runs the probe's shadow
//   pass first on that queue (its own list, allocators and fence; Probe::RecordShadowPass: the last present's top level
//   and cutouts, this frame's camera and lamps). The probe's state is shared with the present under one lock (the pass is
//   skipped while the present holds it).
// - The lights' pass reads it at t50 (the mask) and t51 (its lamps): at PCDX12RenderContext::DrawInstanced, when the
//   descriptor cache's pixel shader ([+0x3490] +0x140 + 4 x 0x568) is the lights' pass's (its counts, +0x60: 59 texture
//   slots (bits 10..16) and 13 constant buffers (bits 17..20): t58, cb12 its highest), the cache's pixel t50 and t51
//   (+0x348 + (4 x 0xAD + slot) x 8) take the probe's CPU descriptors, marked dirty (+0x1C bit 4, +0x5C8 + 4 x 0x568 +
//   slot), so PreDraw copies them into the pass's table; after the draw they are put back.
// - The probe runs while the lamp shadows are on even with its view off (the debug view not drawn then).
// (v0.9.562) The user's run of 0.9.561 ended in DXGI_ERROR_DEVICE_HUNG. The log: the lights' pass was never given the
// mask (the shader test above never held at the draw's start), so the patched pass read t50 / t51 as the engine's
// slots held them; t51 is 14 of the game's shaders' own (a texture or buffer of theirs read as a float4 buffer).
// - Now t45 (the mask) and t46 (its lamps, a 4 x 1 texture: both slots textures), which no shader of the game's declares.
// - How the engine fills a stage's table (FlushCache, 0x30AB4D): for each of the shader's texture slots (its count) whose
//   dirty byte is set, the cache's handle is copied into the context's own staging descriptors (stage x 80 + slot), the
//   byte cleared; then the staging's first count descriptors are copied into the table. So t45 / t46, once taken, stay.
// - At every engine draw (PCDX12RenderContext::DrawInstanced and DrawIndexedInstanced) while the lamp shadows are on, the
//   cache's pixel t45 / t46 are set to the probe's pair (marked dirty only when they change, or when the pair was written
//   again since this thread last set them). The pairs: a heap of the framework's own, never released (pair 0 null
//   descriptors; 1 .. 7 in turn the mask's and the table's, a new one each time the mask is made again).
// - At each D3D12 draw of the lights' pass the cache is checked: its pixel shader's texture count above 46, t45 / t46
//   the probe's and their dirty bytes cleared (copied). The patched pass reads them (ShadowBlend::SetRtLampShadows) only
//   after 64 such draws in a row; a draw without them stops it until the game is started again.
// (v0.9.563) The user's run of 0.9.562: DXGI_ERROR_DEVICE_HUNG as soon as the menu was opened (the lamp shadows on from
// the start; the patched pass never read the mask: its switch-on not logged). Two faults:
// - The probe's frame (RecordFrame: the merge, builds, top level) runs only when the overlay draws a frame (the menu, a
//   world overlay). With the menu closed the threads' capture buffers were never emptied: up to 250,000 draws a thread,
//   the same meshes from some 40 s of frames, merged into one top level when the menu opened (243 ms at the present; the
//   shadow rays through thousands of copies of each mesh). Now each buffer keeps the draws since the last present only
//   (its frame stamp, OnPresent), and while the lamp shadows are on the overlay draws a frame each present (NeedsFrames:
//   an empty one with the menu closed), so the probe's scene follows the game.
// - t45 / t46 were put in for every draw of every shader. The exe's shaders do not use them, but only some of the
//   bigfiles' were looked at: a shader of theirs with its own t45 / t46 read the mask instead. Now they are put in only
//   inside the lights' pass's drawable (cdc::TiledLightingApplyDrawable::Draw, retail 0x2A5F00, TR2 0x1402C51C0: hooked;
//   its draws on that thread), at each draw, and put back after it (the cache's handles as they were, marked dirty, so the
//   next draws copy the engine's own again).
// (v0.9.566, released as 0.9.567: first built into the other session's 0.9.566) The skinned capture's CPU: the user's run
// of 0.9.565 showed nearly all of it (2.5 .. 6.9 ms a frame over
// the threads) in finding and keying the palettes, reading the palette's count and each instance's first 64 bytes from
// the dynamic pool's upload memory (slow for the CPU to read), again for every pass that draws the mesh. The game makes
// and fills a new SkinningBuffer for every draw (ModelDrawable::SetupMatrixState, TR2 0x1402E69E0: a new constant buffer,
// then SetupSkinningMatrices; ModelInstancingDrawable::Draw: one, SetupSkinningMatrices for each instance's
// ModelDrawable), so a buffer's address tells nothing across passes; its drawable does. Now:
// - HookSetupSkinning (SetupSkinningMatrices) notes on its thread the buffer it fills and each instance's drawable.
// - At a skinned draw whose bound buffer is that one (this frame), each instance's drawable is looked up in a table of
//   the poses read this frame (16 shards, each with its lock, emptied when the present count moves on): when all are
//   there, their keys and first bones come from it. (v0.9.567) The buffer's palette count is still read from the buffer itself
//   (one 4-byte read) and must equal the kept one, so what the posing is given (the count, this draw's own GPU address,
//   the instances it holds) is always the buffer's own, as before; the cache only spares the 64 bytes an instance.
// - Otherwise it is read as before and the poses kept for the next passes.
// The keys only merge one frame's passes of a mesh (b.keys, the merge), so a pose's key from its first pass serves all.
// (v0.9.570) Ray-traced sun shadows, the lamps' way:
// - The game's sun shadow-mask pass (20 pixel shader variants, tools/shadow_blend/patch.py) reads the probe's sun mask
//   at t42 (no shader of the game's declares it; under their highest, t43) where its depth agrees, the cascades
//   elsewhere; its marker switched by ShadowBlend::SetRtSunShadows (the pipelines made again: eight_weight_patch.cpp's
//   records, now for this pass too).
// - The pass is drawn by cdc::DirectionalShadowDrawable::Draw (retail 0x2A2E60, TR2 0x1402C2100: hooked); inside it the
//   cache's pixel t42 is the probe's sun descriptor at each engine draw, put back after (as t45 / t46 for the lights).
// - At the pass's D3D12 draws (its pipelines known by EightWeightPatch::IsSunPassPipeline): SceneBuffer (cb2, up to
//   register 86) copied once a frame (the camera; WorldToPSSM0 at 54 .. 57, whose third column is the sun's axis in the
//   world: the sun taken on its side with z up), and the hand-over checked (t42 the probe's, copied, the shader's
//   texture count above 42): the pass reads the mask after 64 such draws in a row; a draw without it stops that.
// - The first submission to the game's queue after that copy runs the sun's pass first (its own list, allocators and
//   fence): the shadow shader in its sun mode (Probe::RecordSunPass): each half-size texel's surface traced up to 250 m,
//   rays toward the sun in a cone of 0.5 degree x the softness (four, two first), then the blur; its own masks.
// (v0.9.571) The sun's cost (the user's run of 0.9.570 at 3840 x 2160: its pass 5 .. 14 ms, the hooks 15 .. 21 ms in a
// crowd): the masks at most 1280 wide; one traversal a sun ray; the sun's surfaces to 150 m (was 250); and while the sun's
// shadows run, characters farther than 160 m from the camera and not near a lamp are left out (they cast no sun shadow
// within its range; were all taken).
// (v0.9.565) The shadow pass cheaper (the user's run of 0.9.564: 3 .. 5 ms of GPU, up to 8, at a 1280 x 720 mask):
// rt_probe_shadow.hlsl traces a texel's first ray only within the lamps' reach (none when it crosses no reach) and casts
// the soft rays two first, the other two only where those disagree. The skinned capture's palette finding timed apart.
// (v0.9.564) The lamp shadows polished (the user's run of 0.9.563 worked: every lights' pass draw given the mask):
// - Soft: each lamp a size from its reach (2 %, 4 .. 30 cm) times the softness (0 .. 3, the menu; 0: one ray, sharp),
//   four rays a lamp to points over it (rt_probe_shadow.hlsl), then a depth-aware blur of the mask (rt_probe_blur.hlsl,
//   2 texels each way; none when sharp) into the mask the lights' pass reads.
// - The lights' pass reads four texels of it, bilinear, each kept only where its depth agrees (light_patch.py).
// - While only the lamp shadows run (the view off), a skinned mesh's instances whose first bone is farther than its
//   reach + 3 m from each of the last lamps are not captured (no capture work, no posing): they cannot shadow them.
// - The shadow pass timed with its own timestamps (the report's "ms on the GPU" for the mask).
#include "rt_probe.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "dx12_overlay.h"
#include "eight_weight_patch.h"
#include "features.h"
#include "game_build.h"
#include "log.h"
#include "rt_probe_gpu.h"
#include "shadow_blend.h"

#include <MinHook.h>
#include <Windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <vector>

namespace RtProbe {
namespace {
constexpr std::uintptr_t kRvaEngineDrawIndexed = 0x00308330ull;
// (v0.9.555) PCDX12RenderContext::DrawInstanced: its prologue as the indexed one's, its call to PreDraw (0x310860)
constexpr std::uintptr_t kRvaEngineDrawInstanced = 0x003083A0ull;
constexpr std::uint8_t kEngineDrawInstancedSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89,
                                                    0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x41, 0x8B, 0xF9, 0x41,
                                                    0x8B, 0xF0, 0x8B, 0xEA, 0x48, 0x8B, 0xD9, 0xE8, 0x9C, 0x84, 0x00, 0x00};
// mov [rsp+8], rbx; mov [rsp+10h], rbp; mov [rsp+18h], rsi; push rdi; sub rsp, 30h; mov edi, r9d; mov esi, r8d;
// mov ebp, edx; mov rbx, rcx; call PreDraw; cmp qword [rbx+3498h], 0
constexpr std::uint8_t kEngineDrawIndexedSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89,
                                                  0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x41, 0x8B, 0xF9, 0x41,
                                                  0x8B, 0xF0, 0x8B, 0xEA, 0x48, 0x8B, 0xD9, 0xE8, 0x0C, 0x85, 0x00, 0x00,
                                                  0x48, 0x83, 0xBB, 0x98, 0x34, 0x00, 0x00, 0x00};
constexpr std::uintptr_t kRvaSetWorldMatrix = 0x002827F0ull;
// movaps xmm2, [rcx+0E60h] ... (its compares and stores at +0xE50..+0xE80; or dword [rcx+0F54h], 40h)
constexpr std::uint8_t kSetWorldMatrixSig[] = {0x0F, 0x28, 0x91, 0x60, 0x0E, 0x00, 0x00};
constexpr std::size_t kCtxWorldMatrix = 0xE50;
// (v0.9.563) cdc::TiledLightingApplyDrawable::Draw (the tiled lights' pass): mov rax, rsp ... sub rsp, 0D0h;
// mov rdi, [RenderDevice::s_pcInstance]
constexpr std::uintptr_t kRvaLightPassDraw = 0x002A5F00ull;
constexpr std::uint8_t kLightPassDrawSig[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
                                              0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00, 0x48,
                                              0x8B, 0x3D, 0x20, 0xDF, 0x7A, 0x01};
// (v0.9.570) cdc::DirectionalShadowDrawable::Draw (the sun's shadow-mask pass): push rbx, rsi, r12, r14, r15;
// sub rsp, 130h; mov r15, [RenderDevice::s_pcInstance]; mov rbx, rcx; mov rcx, r15; mov rax, [r15]
constexpr std::uintptr_t kRvaSunPassDraw = 0x002A2E60ull;
constexpr std::uint8_t kSunPassDrawSig[] = {0x40, 0x53, 0x56, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x30,
                                            0x01, 0x00, 0x00, 0x4C, 0x8B, 0x3D, 0xC9, 0x0F, 0x7B, 0x01, 0x48, 0x8B, 0xD9,
                                            0x49, 0x8B, 0xCF, 0x49, 0x8B, 0x07};
constexpr std::uintptr_t kRvaInstancingDraw = 0x002A4A60ull;   // (v0.9.544) cdc::ModelInstancingDrawable::Draw
constexpr std::uint8_t kInstancingDrawSig[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41,
                                               0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xE0, 0x48, 0x81, 0xEC, 0x20, 0x01,
                                               0x00, 0x00, 0x48, 0x8B, 0xF9};
// mov edx, [rdi+20h] (the count, to SetInstanceCount); mov rcx, [rdi+18h] (the list); mov rbx, [r13+28h]; mov rcx, rsi;
// lea rdx, [rbx+50h] (the first instance's matrix, to SetWorldMatrix)
constexpr std::uintptr_t kRvaInstancingCount = 0x002A4A9Dull, kRvaInstancingList = 0x002A4B15ull,
                         kRvaInstancingMatrix = 0x002A4B6Aull;
constexpr std::uint8_t kInstancingCountSig[] = {0x8B, 0x57, 0x20};
constexpr std::uint8_t kInstancingListSig[] = {0x48, 0x8B, 0x4F, 0x18};
constexpr std::uint8_t kInstancingMatrixSig[] = {0x49, 0x8B, 0x5D, 0x28, 0x48, 0x8B, 0xCE, 0x48, 0x8D, 0x53, 0x50};
constexpr std::size_t kInstList = 0x18, kInstCount = 0x20, kInstRecordMatrix = 0x28, kInstWorld = 0x50;
constexpr unsigned kMaxInstances = 1024;
// cdc::RenderContext::SetSkySphereMode: cmp byte [rcx+0D40h], 0 ... cmp byte [rcx+0D41h], dl (v0.9.544)
constexpr std::uintptr_t kRvaSetSkySphereMode = 0x00281B70ull;
constexpr std::uint8_t kSetSkySphereModeSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x80, 0xB9, 0x40,
                                                 0x0D, 0x00, 0x00, 0x00, 0x0F, 0xB6, 0xFA, 0x48, 0x8B, 0xD9, 0x74, 0x38, 0x38,
                                                 0x91, 0x41, 0x0D, 0x00, 0x00};
constexpr std::size_t kCtxSkyEnabled = 0xD40, kCtxSkyMode = 0xD41;
// (v0.9.545) cdc::ModelDrawable::SetupSkinningMatrices: mov [rsp+18h], rbx; mov [rsp+20h], rdi; push r12; push r14;
// push r15; sub rsp, 10h; mov rax, [rcx+58h]; mov rdi, r9; mov r14, rcx; movaps [rsp], xmm6; mov ecx, 3FFh;
// mov [rsp+30h], rbp; mov rbp, [r8+8] (the cb's data)
constexpr std::uintptr_t kRvaSetupSkinning = 0x002C5A60ull;
constexpr std::uint8_t kSetupSkinningSig[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x54, 0x41, 0x56,
                                              0x41, 0x57, 0x48, 0x83, 0xEC, 0x10, 0x48, 0x8B, 0x41, 0x58, 0x49, 0x8B, 0xF9, 0x4C,
                                              0x8B, 0xF1, 0x0F, 0x29, 0x34, 0x24, 0xB9, 0xFF, 0x03, 0x00, 0x00, 0x48, 0x89, 0x6C,
                                              0x24, 0x30, 0x49, 0x8B, 0x68, 0x08};
constexpr std::size_t kCbData = 0x08;
// (v0.9.546) PCDX12RenderContext::SetConstantBuffer: sub rsp, 38h; movsxd rax, edx; add rax, 0Fh; mov byte [rsp+20h], 0;
// lea r10, [rax+rax*4]; mov eax, r8d; shl r10, 4; add r10, rax; mov [rcx+r10*8], r9
constexpr std::uintptr_t kRvaContextSetCb = 0x00313870ull;
constexpr std::uint8_t kContextSetCbSig[] = {0x48, 0x83, 0xEC, 0x38, 0x48, 0x63, 0xC2, 0x48, 0x83, 0xC0, 0x0F, 0xC6, 0x44, 0x24,
                                             0x20, 0x00, 0x4C, 0x8D, 0x14, 0x80, 0x41, 0x8B, 0xC0, 0x49, 0xC1, 0xE2, 0x04, 0x4C,
                                             0x03, 0xD0, 0x4E, 0x89, 0x0C, 0xD1};
constexpr std::size_t kCtxVertexCb1 = ((0 + 15) * 80 + 1) * 8;   // 0x2588
// (v0.9.552) the context's bound constant buffers and the descriptor cache's texture descriptors (see the top)
constexpr std::size_t kCtxCbs = 0x2580, kCtxDescriptorCache = 0x3490, kCacheSrvs = 0x348, kCacheNulls = 0x2220,
                      kNullSrv = 0x328, kCbSize = 0x14, kCbPool = 0x48, kCbPoolOffset = 0x38;
constexpr unsigned kStageVertex = 0, kStagePixel = 4;
// (v0.9.554) the dynamic pool: RenderDevice::s_pcInstance, its pool, DeviceManager::s_pInstance and its frame slots (as
// PCDX12DynamicConstantBuffer::Map reads them); a block's resource, CPU copy, size and next
constexpr std::uintptr_t kRvaDynamicCbMap = 0x0030FB80ull, kRvaRenderDevice = 0x01A53E40ull,
                         kRvaDeviceManager = 0x01A76B90ull;
constexpr std::uint8_t kDynamicCbMapSig[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20,
    0x41, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0x05, 0x9F, 0x42, 0x74, 0x01, 0x4C, 0x8D, 0x71, 0x48, 0x8B, 0x51, 0x14,
    0x4C, 0x8D, 0x49, 0x38, 0x48, 0x8B, 0x1D, 0xDD, 0x6F, 0x76, 0x01, 0x48, 0x8B, 0xF1, 0x41, 0xB8, 0x00, 0x01, 0x00, 0x00,
    0x4C, 0x89, 0x74, 0x24, 0x20, 0x48, 0x8B, 0x88, 0xB8, 0xB4, 0x02, 0x00};
constexpr std::size_t kDevicePool = 0x2B4B8, kManagerFrames = 0x820, kPoolLists = 0x10, kPoolListStride = 0x30,
                      kBlockResource = 0x08, kBlockCpu = 0x10, kBlockSize = 0x20, kBlockNext = 0x30;
bool g_poolCheck = false;
constexpr unsigned kMaxPalette = 1023, kMaxSkinnedVertices = 300000;
constexpr std::size_t kSlotDrawIndexed = 13, kSlotTopology = 20, kSlotIndexBuffer = 43, kSlotVertexBuffers = 44;
constexpr std::size_t kMaxDraws = 250000;

using EngineDrawIndexedFn = void(__fastcall*)(void* ctx, unsigned indexCount, unsigned instances, unsigned startIndex,
                                              int baseVertex, unsigned startInstance);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using EngineDrawInstancedFn = void(__fastcall*)(void* ctx, unsigned vertexCount, unsigned instances, unsigned startVertex,
                                                unsigned startInstance);   // (v0.9.555)
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
EngineDrawInstancedFn g_originalEngineDrawInstanced = nullptr;
DrawInstancedFn g_realDrawInstanced = nullptr;
constexpr std::size_t kSlotDrawInstanced = 12;
using TopologyFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_PRIMITIVE_TOPOLOGY);
using IndexBufferFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_INDEX_BUFFER_VIEW*);
using VertexBuffersFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);

EngineDrawIndexedFn g_originalEngineDraw = nullptr;
using InstancingDrawFn = void(__fastcall*)(void* drawable, unsigned pass, void* other);
InstancingDrawFn g_originalInstancingDraw = nullptr;
InstancingDrawFn g_originalLightPassDraw = nullptr;   // (v0.9.563) the same signature: (this, pass, other drawable)
thread_local unsigned t_lightPassDepth = 0;           // (v0.9.563) inside the lights' pass's drawable on this thread
InstancingDrawFn g_originalSunPassDraw = nullptr;     // (v0.9.570) the sun's shadow pass's drawable
thread_local unsigned t_sunPassDepth = 0;             // (v0.9.570) inside it on this thread
std::atomic<std::uint32_t> g_presentCount{0};         // (v0.9.563) presents so far (the capture buffers' frame stamp)
using SetupSkinningFn = void(__fastcall*)(void* drawable, void* ctx, void* cb, void* previous, int instance);
SetupSkinningFn g_originalSetupSkinning = nullptr;   // (v0.9.545)
bool g_skyCheck = false;
DrawIndexedFn g_realDrawIndexed = nullptr;
TopologyFn g_realTopology = nullptr;
IndexBufferFn g_realIndexBuffer = nullptr;
VertexBuffersFn g_realVertexBuffers = nullptr;
void** g_hookedVtable = nullptr;
std::mutex g_attachLock;
bool g_installed = false;

std::atomic<int> g_mode{0};
std::atomic<bool> g_lampShadows{false};   // (v0.9.561)
std::atomic<bool> g_sunShadows{false};    // (v0.9.570) ray-traced sun shadows
std::atomic<float> g_lampSoftness{1.0f};   // (v0.9.564) 0 sharp .. 3
// (v0.9.564) the lamps the last shadow setup chose (world position, reach), for leaving out the skinned meshes far from
// all of them while only the lamp shadows run; each thread's copy taken once a frame
constexpr float kCullMargin = 300.0f;
std::mutex g_cullLock;
float g_cullLamps[3][4] = {};
unsigned g_cullCount = 0;
bool g_cullKnown = false;
float g_cullCamera[4] = {};   // (v0.9.571) the sun's: the camera, and how far from it characters are taken (0: not known)
std::atomic<std::uint32_t> g_skinnedFar{0};
struct CullLamps {
    std::uint32_t frame = 0xFFFFFFFFu;
    float lamps[3][4] = {};
    unsigned count = 0;
    bool known = false;
    float camera[4] = {};   // (v0.9.571)
};
thread_local CullLamps t_cull;
thread_local std::vector<std::uint8_t> t_near;   // this draw's instances kept
// whether a skinned mesh whose first bone is here can matter (true: the view is on, or no lamps chosen yet)
bool NearLamps(const float* origin) {
    // (v0.9.570) every one while the sun's shadows run too (they cast the sun's); (v0.9.571) those within the sun's
    // range of the camera
    if (g_mode.load(std::memory_order_relaxed) != 0) return true;
    const bool sun = g_sunShadows.load(std::memory_order_relaxed);
    const std::uint32_t frame = g_presentCount.load(std::memory_order_relaxed);
    if (t_cull.frame != frame) {
        std::lock_guard<std::mutex> lock(g_cullLock);
        std::memcpy(t_cull.lamps, g_cullLamps, sizeof(t_cull.lamps));
        t_cull.count = g_cullCount;
        t_cull.known = g_cullKnown;
        std::memcpy(t_cull.camera, g_cullCamera, sizeof(t_cull.camera));
        t_cull.frame = frame;
    }
    if (sun) {   // (v0.9.571)
        if (!(t_cull.camera[3] > 0.0f)) return true;
        const float dx = origin[0] - t_cull.camera[0], dy = origin[1] - t_cull.camera[1], dz = origin[2] - t_cull.camera[2];
        if (dx * dx + dy * dy + dz * dz < t_cull.camera[3] * t_cull.camera[3]) return true;
        if (!g_lampShadows.load(std::memory_order_relaxed)) return false;
    }
    if (!t_cull.known) return true;
    for (unsigned k = 0; k < t_cull.count && k < 3; ++k) {
        const float* l = t_cull.lamps[k];
        const float dx = origin[0] - l[0], dy = origin[1] - l[1], dz = origin[2] - l[2], r = l[3] + kCullMargin;
        if (dx * dx + dy * dy + dz * dz < r * r) return true;
    }
    return false;
}
// (v0.9.562) the lights' pass's t45 / t46 (see the top): the published pair's first CPU handle (0: none), the heap's
// increment, a count raised each time a pair is written; the lights' pass draws checked (given, not given), the run of
// draws given, the last pixel shader counts seen; whether the patched pass reads them
std::atomic<std::uint64_t> g_lightHandle{0};
std::atomic<unsigned> g_lightIncrement{0};
std::atomic<std::uint32_t> g_lightEpoch{0};
std::atomic<std::uint32_t> g_maskInjected{0}, g_lightPassUnmatched{0};   // (v0.9.561)
std::atomic<std::uint32_t> g_givenStreak{0}, g_lightMeta{0}, g_notGivenLogs{0};
enum : int { kReadWaiting = 0, kReadOn = 1, kReadStopped = 2 };
std::atomic<int> g_rtRead{kReadWaiting};
// (v0.9.570) the sun's: the published descriptor (0: none), the sun pass's draws checked, the run given, the counts
// seen, whether its patched pass reads it
std::atomic<std::uint64_t> g_sunHandle{0};
std::atomic<std::uint32_t> g_sunGiven{0}, g_sunNotGiven{0}, g_sunStreak{0}, g_sunMeta{0}, g_sunNotGivenLogs{0};
std::atomic<int> g_sunRead{kReadWaiting};
struct SunSaved {   // (v0.9.570) the cache's pixel t42 before the probe's was put in
    std::uint8_t* cache;
    std::uint64_t handle;
};
SunSaved GiveSunPass(void* ctx) noexcept;
void RestoreSunPass(const SunSaved& saved) noexcept;
void CaptureSun(const void* ctx);
void CheckSunPass(void* ctx);
struct LightSaved {   // (v0.9.563) the cache's pixel t45 / t46 before the probe's were put in
    std::uint8_t* cache;
    std::uint64_t handles[2];
};
LightSaved GiveLightPass(void* ctx) noexcept;        // (at the engine's draws inside the lights' pass's drawable)
void RestoreLightPass(const LightSaved& saved) noexcept;
void CheckLightPass(void* ctx);                      // (at the lights' pass's D3D12 draws)
// (v0.9.561) the probe captures while its view or the lamp shadows are on
bool Running() {
    return g_mode.load(std::memory_order_relaxed) != 0 || g_lampShadows.load(std::memory_order_relaxed) ||
           g_sunShadows.load(std::memory_order_relaxed);   // (v0.9.570)
}
thread_local void* t_engineContext = nullptr;
// (v0.9.544) the instancing drawable being drawn on this thread: its instances' world matrices (16 floats each)
thread_local std::vector<float> t_instanceMatrices;
thread_local unsigned t_instanceCount = 0;
// (v0.9.545) a palette (64 bytes an entry) being read or copied on this thread
thread_local std::vector<std::uint8_t> t_palette;
// (v0.9.546) the palettes by the constant buffer SetupSkinningMatrices wrote them into
struct KeptPalette {
    std::vector<std::uint8_t> bytes;    // (v0.9.550) the instances' palettes one after another (entries x 64 bytes each)
    unsigned entries = 0;
    std::vector<std::uint64_t> hashes;  // (v0.9.550) one an instance
};
thread_local std::vector<std::uint64_t> t_paletteHashes;   // (v0.9.550)
thread_local std::vector<float> t_paletteOrigins;          // (v0.9.559) three an instance
std::mutex g_paletteLock;
std::unordered_map<const void*, KeptPalette> g_palettes;
bool g_boundCbCheck = false;

// the command lists' bound state (per thread: an engine context records on one thread at a time)
struct ListState {
    ID3D12GraphicsCommandList* list = nullptr;
    EightWeightPatch::PipelineGeometry pipeline{};
    D3D12_VERTEX_BUFFER_VIEW vb[4]{};
    D3D12_INDEX_BUFFER_VIEW ib{};
    D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    bool lightPass = false;   // (v0.9.555) the bound pipeline is the tiled lights' pass's
    bool sunPass = false;     // (v0.9.570) the sun's shadow pass's
};
thread_local ListState t_lists[4];
thread_local unsigned t_nextList = 0;

ListState& StateFor(ID3D12GraphicsCommandList* list) {
    for (ListState& s : t_lists)
        if (s.list == list) return s;
    ListState& s = t_lists[t_nextList++ % 4];
    s = ListState{};
    s.list = list;
    return s;
}
bool BoundSunPass(ID3D12GraphicsCommandList* list) {   // (v0.9.570) as StateFor(list).sunPass, none made
    for (const ListState& s : t_lists)
        if (s.list == list) return s.sunPass;
    return false;
}
bool BoundLightPass(ID3D12GraphicsCommandList* list) {   // (v0.9.562) as StateFor(list).lightPass, none made
    for (const ListState& s : t_lists)
        if (s.list == list) return s.lightPass;
    return false;
}

struct Draw {
    std::uint64_t key;
    RtProbeGpu::Geometry geometry;
    float transform[3][4];
    std::uint8_t kind = 0;   // (v0.9.548) 0 static, 1 instanced, 2 a skinned layout drawn rigid, 5 tessellated
    bool cut = false;        // (v0.9.552) its alpha test resolved
    bool uncertain = false;  // (v0.9.570) an alpha test there, not resolved (traced as solid)
    RtProbeGpu::Cutout cutout;
};

// counters (since the last report)
std::atomic<std::uint32_t> g_seen{0}, g_taken{0}, g_skinned{0}, g_instanced{0}, g_format{0}, g_topology{0},
    g_noDepth{0}, g_noPipeline{0}, g_noBuffers{0}, g_dropped{0};
std::atomic<std::uint32_t> g_instancedTaken{0}, g_sky{0}, g_tessellated{0};   // (v0.9.544)
std::atomic<std::uint32_t> g_skinnedTaken{0}, g_skinned8{0}, g_skinnedNoPalette{0}, g_skinnedFormat{0},
    g_skinnedRepeat{0};   // (v0.9.545)
std::atomic<std::uint32_t> g_cutTaken{0}, g_cutUnresolved{0}, g_cutUnfollowed{0};   // (v0.9.552)
std::atomic<std::uint32_t> g_skinnedFromInstanced{0}, g_skinnedShort{0}, g_skinnedTopology{0}, g_skinnedNoDepth{0},
    g_skinnedOnlyNoDepth{0};   // (v0.9.550)

// (v0.9.545) the frame's skinned meshes (under g_drawLock): each mesh and palette once
struct SkinnedDraw {
    std::uint64_t key;
    RtProbeGpu::SkinnedGeometry geometry;
    std::size_t paletteAt;
    unsigned paletteEntries;
    std::uint8_t kind = 3;   // (v0.9.548) 3 four-weight, 4 one bone; (v0.9.550) 6 an instance, 7 drawn only without depth
    bool cut = false;        // (v0.9.552) its alpha test resolved
    bool uncertain = false;  // (v0.9.570) an alpha test there, not resolved
    RtProbeGpu::Cutout cutout;
    const std::uint8_t* palette = nullptr;   // (v0.9.558) set at the merge: its palette in its thread's spare buffer
    D3D12_GPU_VIRTUAL_ADDRESS paletteGpu = 0;   // (v0.9.559) or the game's buffer, posed from where it is
    float origin[3] = {};                       // (v0.9.559) its first entry's translation (the distance's)
};
std::atomic<bool> g_kindView{false};   // (v0.9.548)
std::atomic<bool> g_shadowMask{false};   // (v0.9.560)
RtProbeGpu::ShadowSetup g_lastShadow;    // (v0.9.560) the last frame's (the present thread)
std::uint32_t g_kindCounts[8] = {};   // the last frame's objects by kind (the present thread)
float g_skinnedFarthest = 0.0f;
std::uint64_t g_cpuFrameTicks = 0, g_cpuRecordTicks = 0;   // (v0.9.555) the present thread's, since the last report   // (v0.9.551) the last frame's farthest skinned mesh posed (-1: none with a distance)
// (v0.9.558) a recording thread's captures (its own lock: uncontended but for the present's swap); the present swaps
// each list with its spare (kept empty, its capacity kept) and reads the spares
struct CaptureBuffer {
    std::mutex lock;
    std::vector<Draw> draws, spareDraws;
    std::vector<SkinnedDraw> skinned, spareSkinned;   // the passes that write depth: each mesh and pose once a thread
    // (v0.9.550) the skinned draws without depth writes (taken at the frame's end if no pass that writes depth gives them)
    std::vector<SkinnedDraw> later, spareLater;
    std::vector<std::uint8_t> palettes, sparePalettes;   // the skinned draws' palettes (paletteAt into these)
    std::unordered_set<std::uint64_t> keys, laterKeys;
    std::uint32_t frame = 0;   // (v0.9.563) the present count its contents were captured after
};
// (v0.9.563) with its lock held, before adding: a buffer from before the last present emptied (not merged: no probe frame
// ran), so it holds one frame at most
void Freshen(CaptureBuffer& b) {
    const std::uint32_t now = g_presentCount.load(std::memory_order_relaxed);
    if (b.frame == now) return;
    b.frame = now;
    b.draws.clear();
    b.skinned.clear();
    b.later.clear();
    b.palettes.clear();
    b.keys.clear();
    b.laterKeys.clear();
}
std::mutex g_buffersLock;
std::vector<CaptureBuffer*> g_buffers;   // every recording thread's (kept: the game's threads are few)
thread_local CaptureBuffer* t_buffer = nullptr;

CaptureBuffer& ThreadBuffer() {
    if (!t_buffer) {
        auto* buffer = new CaptureBuffer();
        std::lock_guard<std::mutex> lock(g_buffersLock);
        g_buffers.push_back(buffer);
        t_buffer = buffer;
    }
    return *t_buffer;
}
std::atomic<std::uint32_t> g_formatCounts[256] = {};   // position formats seen (taken or not); (v0.9.556) an array

std::uint64_t Hash(const void* data, std::size_t size, std::uint64_t h = 1469598103934665603ull) {
    const auto* b = static_cast<const std::uint8_t*>(data);   // (v0.9.558) eight bytes at a time, then the rest
    std::size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        std::uint64_t w;
        std::memcpy(&w, b + i, 8);
        h = (h ^ w) * 1099511628211ull;
        h ^= h >> 29;
    }
    for (; i < size; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

bool ReadWorld(void* ctx, float m[4][4]) noexcept {
    __try {
        std::memcpy(m, static_cast<std::uint8_t*>(ctx) + kCtxWorldMatrix, sizeof(float) * 16);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (!std::isfinite(m[r][c]) || std::fabs(m[r][c]) > 1.0e8f) return false;
    return true;
}

// (v0.9.544) The instancing drawable's instances: their world matrices into out (16 floats each); 0 when unreadable or
// more than max.
unsigned ReadInstances(void* drawable, float* out, unsigned max) noexcept {
    __try {
        auto* d = static_cast<std::uint8_t*>(drawable);
        const unsigned count = *reinterpret_cast<std::uint32_t*>(d + kInstCount);
        auto* const* records = *reinterpret_cast<std::uint8_t* const* const*>(d + kInstList);
        if (!count || count > max || !records) return 0;
        for (unsigned i = 0; i < count; ++i) {
            const std::uint8_t* record = records[i];
            if (!record) return 0;
            const std::uint8_t* matrix = *reinterpret_cast<const std::uint8_t* const*>(record + kInstRecordMatrix);
            if (!matrix) return 0;
            std::memcpy(out + i * 16, matrix + kInstWorld, sizeof(float) * 16);
        }
        return count;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool SkyDraw(void* ctx) noexcept {
    __try {
        const auto* c = static_cast<const std::uint8_t*>(ctx);
        return c[kCtxSkyEnabled] && c[kCtxSkyMode];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool UsableMatrix(const float* m) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i]) || std::fabs(m[i]) > 1.0e8f) return false;
    return true;
}

// (v0.9.546) The constant buffer bound as the vertex shader's b1 on the engine's context (null when unreadable)
// (v0.9.566) the palettes' setups on this thread: the buffer filled last (this frame) and each instance's drawable in it
thread_local const void* t_setupCb = nullptr;
thread_local std::uint32_t t_setupFrame = 0;
thread_local std::vector<const void*> t_setupDrawables;
// the poses read this frame, by drawable: their key, first bone (origin) and entries
struct PoseSeen {
    std::uint64_t key;
    float origin[3];
    unsigned entries;
};
struct PoseShard {
    std::mutex lock;
    std::uint32_t frame = 0xFFFFFFFFu;
    std::unordered_map<const void*, PoseSeen> seen;
};
PoseShard g_poses[16];
std::atomic<std::uint32_t> g_posesRead{0}, g_posesKnown{0};   // (v0.9.566) instances read, instances found already read
PoseShard& ShardFor(const void* drawable) {
    const auto v = reinterpret_cast<std::uintptr_t>(drawable);
    return g_poses[((v >> 4) ^ (v >> 12)) & 15];
}
bool FindPose(const void* drawable, std::uint32_t frame, PoseSeen& out) {
    PoseShard& shard = ShardFor(drawable);
    std::lock_guard<std::mutex> lock(shard.lock);
    if (shard.frame != frame) return false;
    const auto it = shard.seen.find(drawable);
    if (it == shard.seen.end()) return false;
    out = it->second;
    return true;
}
void KeepPose(const void* drawable, std::uint32_t frame, const PoseSeen& pose) {
    PoseShard& shard = ShardFor(drawable);
    std::lock_guard<std::mutex> lock(shard.lock);
    if (shard.frame != frame) {
        shard.seen.clear();
        shard.frame = frame;
    }
    if (shard.seen.size() < 8192) shard.seen[drawable] = pose;
}
const std::uint8_t* DynamicData(const void* resource, std::uint64_t offset, std::uint32_t bytes) noexcept;   // (below)
// (v0.9.566) the bound SkinningBuffer's place, its data not read: its size, its static copy (or null) and, for a dynamic
// one, its first palette's GPU address
bool BoundPaletteSource(const void* ctx, const void*& cbOut, std::uint32_t& size, const std::uint8_t*& staticData,
                        D3D12_GPU_VIRTUAL_ADDRESS& gpu, unsigned& entries) noexcept {
    cbOut = nullptr;
    size = 0;
    staticData = nullptr;
    gpu = 0;
    entries = 0;   // its palette count, read from the buffer (its first 4 bytes; 0: not readable)
    if (!ctx) return false;
    __try {
        const auto* cb = *reinterpret_cast<const std::uint8_t* const*>(static_cast<const std::uint8_t*>(ctx) + kCtxVertexCb1);
        if (!cb) return false;
        cbOut = cb;
        size = *reinterpret_cast<const std::uint32_t*>(cb + kCbSize);
        staticData = *reinterpret_cast<const std::uint8_t* const*>(cb + kCbData);
        const std::uint8_t* data = staticData;
        if (!staticData) {
            auto* resource = *reinterpret_cast<ID3D12Resource* const*>(cb + kCbPool);
            if (!resource) return false;
            const UINT64 offset = *reinterpret_cast<const UINT64*>(cb + kCbPoolOffset);
            data = DynamicData(resource, offset, 16);
            gpu = resource->GetGPUVirtualAddress() + offset + 16;
        }
        if (!data || size < 16 + 64) return false;
        const float count = *reinterpret_cast<const float*>(data);
        if (!(count >= 1.0f) || count > static_cast<float>(kMaxPalette)) return false;
        entries = static_cast<unsigned>(count);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const void* BoundSkinningBuffer(void* ctx) noexcept {
    if (!ctx) return nullptr;
    __try {
        return *reinterpret_cast<const void* const*>(static_cast<const std::uint8_t*>(ctx) + kCtxVertexCb1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// (v0.9.553) Where a constant read stopped (for the log)
struct ConstantTrace {
    const void* cb = nullptr;
    std::uint32_t size = 0;
    const void* data = nullptr;
    const void* pool = nullptr;
    std::uint64_t offset = 0;
    long mapResult = 0;
    int step = 0;   // 0 read; 1 no context or slot; 2 no buffer; 3 past its size; 4 not finite; 5 no resource; 6 not in the
                    // dynamic pool's blocks; 7 fault
};

// (v0.9.554) A dynamic buffer's data: the dynamic pool's block holding its resource, that block's CPU copy at the offset
// (null: not found, the pool not checked)
struct PoolBlockSeen {   // (v0.9.556) a block found, kept per thread
    const void* resource;
    const std::uint8_t* block;
};
thread_local PoolBlockSeen t_poolBlocks[32] = {};   // (v0.9.558: 32, was 8)
thread_local unsigned t_poolNext = 0;
std::atomic<std::uint32_t> g_poolMisses{0}, g_poolWalked{0};   // (v0.9.558) lookups past the thread's blocks, blocks walked

const std::uint8_t* DynamicData(const void* resource, std::uint64_t offset, std::uint32_t bytes) noexcept {
    if (!g_poolCheck || !resource) return nullptr;
    __try {
        for (unsigned k = 0; k < 32; ++k) {   // (v0.9.556) this thread's recent blocks first (still that resource's)
            const std::uint8_t* block = t_poolBlocks[k].block;
            if (t_poolBlocks[k].resource != resource || !block ||
                *reinterpret_cast<const void* const*>(block + kBlockResource) != resource)
                continue;
            const auto* cpu = *reinterpret_cast<const std::uint8_t* const*>(block + kBlockCpu);
            const std::uint64_t size = *reinterpret_cast<const std::uint64_t*>(block + kBlockSize);
            if (cpu && offset + bytes <= size) return cpu + offset;
        }
        const auto* device = *reinterpret_cast<const std::uint8_t* const*>(GameBuild::Base() + kRvaRenderDevice);
        const auto* manager = *reinterpret_cast<const std::uint8_t* const*>(GameBuild::Base() + kRvaDeviceManager);
        if (!device || !manager) return nullptr;
        const auto* pool = *reinterpret_cast<const std::uint8_t* const*>(device + kDevicePool);
        if (!pool) return nullptr;
        std::uint32_t slots = *reinterpret_cast<const std::uint32_t*>(manager + kManagerFrames);
        if (slots > 8) slots = 8;
        g_poolMisses.fetch_add(1, std::memory_order_relaxed);   // (v0.9.558)
        std::uint32_t walked = 0;
        for (std::uint32_t i = 0; i < slots; ++i)
            for (std::uint32_t list = 0; list < 2; ++list) {
                const auto* block =
                    *reinterpret_cast<const std::uint8_t* const*>(pool + kPoolLists + list * 8 + i * kPoolListStride);
                for (int n = 0; block && n < 4096; ++n) {
                    if (*reinterpret_cast<const void* const*>(block + kBlockResource) == resource) {
                        const auto* cpu = *reinterpret_cast<const std::uint8_t* const*>(block + kBlockCpu);
                        const std::uint64_t size = *reinterpret_cast<const std::uint64_t*>(block + kBlockSize);
                        if (cpu && offset + bytes <= size) {
                            t_poolBlocks[t_poolNext++ & 31] = {resource, block};   // (v0.9.556)
                            g_poolWalked.fetch_add(walked, std::memory_order_relaxed);
                            return cpu + offset;
                        }
                    }
                    ++walked;
                    block = *reinterpret_cast<const std::uint8_t* const*>(block + kBlockNext);
                }
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

// (v0.9.556) The SkinningBuffer bound as the vertex shader's b1, read in place: its first palette (its static copy, or
// the dynamic pool's CPU copy), the entries a palette and how many instances' palettes it holds (null: none, unreadable)
bool g_directPalettes = false;   // the pool there: palettes read in place (else SetupSkinningMatrices' copies)
const std::uint8_t* BoundPalettes(const void* ctx, unsigned& entries, unsigned& held, D3D12_GPU_VIRTUAL_ADDRESS& gpu) noexcept {
    entries = held = 0;
    gpu = 0;   // (v0.9.559) a dynamic buffer's first palette on the GPU (0: a static one, read on the CPU)
    if (!ctx) return nullptr;
    __try {
        const auto* cb = *reinterpret_cast<const std::uint8_t* const*>(static_cast<const std::uint8_t*>(ctx) + kCtxVertexCb1);
        if (!cb) return nullptr;
        const std::uint32_t size = *reinterpret_cast<const std::uint32_t*>(cb + kCbSize);
        if (size < 16 + 64) return nullptr;
        const auto* data = *reinterpret_cast<const std::uint8_t* const*>(cb + kCbData);
        if (!data) {
            auto* resource = *reinterpret_cast<ID3D12Resource* const*>(cb + kCbPool);
            const UINT64 offset = *reinterpret_cast<const UINT64*>(cb + kCbPoolOffset);
            data = DynamicData(resource, offset, 16 + 64);   // (v0.9.559) its count and first entry only
            if (data && resource) gpu = resource->GetGPUVirtualAddress() + offset + 16;
        }
        if (!data) {
            gpu = 0;
            return nullptr;
        }
        const float count = *reinterpret_cast<const float*>(data);
        if (!(count >= 1.0f) || count > static_cast<float>(kMaxPalette)) return nullptr;
        entries = static_cast<unsigned>(count);
        held = (size - 16) / (entries * 64);
        return data + 16;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        entries = held = 0;
        gpu = 0;
        return nullptr;
    }
}

// (v0.9.556) a palette's key; (v0.9.559) from its entries and its first entry only (the root places the pose; read once,
// 64 bytes: the pool's CPU copy is slow to read), and that entry's translation (its columns' fourth components)
std::uint64_t PaletteKey(const std::uint8_t* palette, unsigned entries, float origin[3]) noexcept {
    std::uint64_t h = 1469598103934665603ull ^ entries;
    origin[0] = origin[1] = origin[2] = 0.0f;
    __try {
        std::uint64_t w[8];
        std::memcpy(w, palette, sizeof(w));
        for (int i = 0; i < 8; ++i) h = (h ^ w[i]) * 1099511628211ull;
        float e[12];
        std::memcpy(e, w, sizeof(e));
        origin[0] = e[3];
        origin[1] = e[7];
        origin[2] = e[11];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return h;
}

bool CopySafe(void* to, const void* from, std::size_t bytes) noexcept {   // (v0.9.556)
    __try {
        std::memcpy(to, from, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// (v0.9.555) The first bytes (at most max) of the constant buffer bound to the context's stage and slot; its size in
// *size (0: none or unreadable)
unsigned ReadConstantBuffer(const void* ctx, unsigned stage, unsigned slot, void* out, unsigned max, unsigned* size) noexcept {
    if (size) *size = 0;
    if (!ctx || slot >= 16) return 0;
    __try {
        const auto* base = static_cast<const std::uint8_t*>(ctx);
        const auto* cb = *reinterpret_cast<const std::uint8_t* const*>(base + kCtxCbs + (stage * 80 + slot) * 8);
        if (!cb) return 0;
        const std::uint32_t bytes = *reinterpret_cast<const std::uint32_t*>(cb + kCbSize);
        if (size) *size = bytes;
        const unsigned n = bytes < max ? bytes : max;
        const auto* data = *reinterpret_cast<const std::uint8_t* const*>(cb + kCbData);
        if (!data) {
            const void* resource = *reinterpret_cast<const void* const*>(cb + kCbPool);
            const std::uint64_t offset = *reinterpret_cast<const UINT64*>(cb + kCbPoolOffset);
            data = DynamicData(resource, offset, n);
        }
        if (!data) return 0;
        std::memcpy(out, data, n);
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// (v0.9.555) The lights' pass's lights (pixel cb12) and SceneBuffer's View (pixel cb2 +0), copied at the first such draw
// of a frame
constexpr unsigned kLightRegisters = 12, kMaxLights = 256;
struct LightSnapshot {
    std::uint64_t frame = ~0ull;
    unsigned lightBytes = 0, lightBufferSize = 0, sceneBytes = 0;
    float lights[kMaxLights * kLightRegisters * 4] = {};
    float scene[87 * 4] = {};   // (v0.9.560) SceneBuffer up to register 86 (View at 0; [18], [45], [46], [85])
    std::uint32_t draws = 0;   // the lights' pass's draws since the last report
};
std::mutex g_lightLock;
LightSnapshot g_lights;
std::atomic<std::uint64_t> g_frameNumber{0};
std::atomic<std::uint64_t> g_lightSeq{0}, g_shadowSeqDone{0};   // (v0.9.561) the lights' copies, the last shadow pass's
// (v0.9.555) the hooks' time (all threads) since the last report; (v0.9.557) per part
// (v0.9.565) kPartSkinFind: inside the skinned capture, finding its palettes and keying them (before the lamp culling)
enum HookPart : int { kPartPipeline, kPartDraw, kPartSkinned, kPartCutout, kPartInstancing, kPartPalette, kPartSkinFind,
                      kParts };
std::atomic<std::uint64_t> g_cpuPartTicks[kParts] = {};

// (v0.9.570) the sun's pass: SceneBuffer at its first draw of a frame (the camera, WorldToPSSM0); a sun pass due
struct SunSnapshot {
    std::uint64_t frame = ~0ull;
    unsigned sceneBytes = 0;
    float scene[87 * 4] = {};
    std::uint32_t draws = 0;
};
std::mutex g_sunLock;
SunSnapshot g_sun;
std::atomic<std::uint64_t> g_sunSeq{0}, g_sunSeqDone{0};
std::atomic<std::uint32_t> g_sunPasses{0}, g_sunBusy{0};
void CaptureSun(const void* ctx) {
    std::lock_guard<std::mutex> lock(g_sunLock);
    ++g_sun.draws;
    const std::uint64_t frame = g_frameNumber.load(std::memory_order_relaxed);
    if (g_sun.frame == frame) return;
    g_sun.frame = frame;
    g_sun.sceneBytes = ReadConstantBuffer(ctx, kStagePixel, 2, g_sun.scene, sizeof(g_sun.scene), nullptr);
    g_sunSeq.fetch_add(1, std::memory_order_release);
}

void CaptureLights(const void* ctx) {
    std::lock_guard<std::mutex> lock(g_lightLock);
    ++g_lights.draws;
    const std::uint64_t frame = g_frameNumber.load(std::memory_order_relaxed);
    if (g_lights.frame == frame) return;
    g_lights.frame = frame;
    g_lights.lightBytes = ReadConstantBuffer(ctx, kStagePixel, 12, g_lights.lights, sizeof(g_lights.lights),
                                             &g_lights.lightBufferSize);
    g_lights.sceneBytes = ReadConstantBuffer(ctx, kStagePixel, 2, g_lights.scene, sizeof(g_lights.scene), nullptr);
    g_lightSeq.fetch_add(1, std::memory_order_release);   // (v0.9.561) a shadow pass due at the next submission
}

struct HookTimer {   // (v0.9.555) a hook's own time; (v0.9.557) by part
    LARGE_INTEGER start;
    int part;
    explicit HookTimer(int which) : part(which) { QueryPerformanceCounter(&start); }
    ~HookTimer() {
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        g_cpuPartTicks[part].fetch_add(static_cast<std::uint64_t>(end.QuadPart - start.QuadPart), std::memory_order_relaxed);
    }
};

// (v0.9.552) A float of the constant buffer bound to the context's stage (a literal as itself)
bool ReadConstant(const void* ctx, unsigned stage, const RtCutout::Constant& c, float& out, ConstantTrace& t) noexcept {
    t = ConstantTrace{};
    if (c.slot < 0) {
        out = c.literal;
        return true;
    }
    if (!ctx || c.slot >= 16) {
        t.step = 1;
        return false;
    }
    __try {
        const auto* base = static_cast<const std::uint8_t*>(ctx);
        const auto* cb = *reinterpret_cast<const std::uint8_t* const*>(base + kCtxCbs + (stage * 80 + c.slot) * 8);
        t.cb = cb;
        if (!cb) {
            t.step = 2;
            return false;
        }
        t.size = *reinterpret_cast<const std::uint32_t*>(cb + kCbSize);
        const auto* data = *reinterpret_cast<const std::uint8_t* const*>(cb + kCbData);
        t.data = data;
        if ((c.at + 1u) * 4u > t.size) {
            t.step = 3;
            return false;
        }
        if (data) {
            std::memcpy(&out, data + c.at * 4u, 4);
            t.step = std::isfinite(out) ? 0 : 4;
            return t.step == 0;
        }
        // a dynamic one, unmapped: (v0.9.554) its resource's block's CPU copy in the dynamic pool, at its offset (the
        // resource itself is not CPU visible: Map refuses it)
        const void* resource = *reinterpret_cast<const void* const*>(cb + kCbPool);
        t.pool = resource;
        t.offset = *reinterpret_cast<const UINT64*>(cb + kCbPoolOffset);
        if (!resource) {
            t.step = 5;
            return false;
        }
        const std::uint8_t* copy = DynamicData(resource, t.offset, (c.at + 1u) * 4u);
        if (!copy) {
            t.step = 6;
            return false;
        }
        std::memcpy(&out, copy + c.at * 4u, 4);
        t.step = std::isfinite(out) ? 0 : 4;
        return t.step == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        t.step = 7;
        return false;
    }
}

// (v0.9.552) The CPU descriptor of the texture bound to the pixel shader's slot (0: none, or the null one)
std::uint64_t PixelTexture(const void* ctx, unsigned slot, std::uint64_t* rawHandle = nullptr) noexcept {
    if (rawHandle) *rawHandle = 0;
    if (!ctx || slot >= 64) return 0;
    __try {
        const auto* cache = *reinterpret_cast<const std::uint8_t* const*>(static_cast<const std::uint8_t*>(ctx) +
                                                                        kCtxDescriptorCache);
        if (!cache) return 0;
        const std::uint64_t handle =
            *reinterpret_cast<const std::uint64_t*>(cache + kCacheSrvs + (kStagePixel * 0xAD + slot) * 8);
        if (rawHandle) *rawHandle = handle;
        const auto* nulls = *reinterpret_cast<const std::uint8_t* const*>(cache + kCacheNulls);
        const std::uint64_t none = nulls ? *reinterpret_cast<const std::uint64_t*>(nulls + kNullSrv) : 0;
        return handle == none ? 0 : handle;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// (v0.9.553) why a draw's alpha test was not resolved (0: resolved)
enum CutFail : int {
    kCutOk = 0, kCutNoTexcoord, kCutUvFormat, kCutUvView, kCutUvStride, kCutTexture, kCutVsScale, kCutVsOffset,
    kCutTiling, kCutOffset, kCutThreshold, kCutReasons
};
const char* const kCutFailNames[kCutReasons] = {
    "resolved", "no TEXCOORD in the layout", "its UV format", "no UV view", "UV past the stride", "no texture",
    "vertex scale", "vertex offset", "tiling", "offset", "threshold"};
std::atomic<std::uint32_t> g_cutFail[kCutReasons] = {};
std::atomic<int> g_cutSamples[kCutReasons] = {};

std::string DescribeConstant(const RtCutout::Constant& c) {
    char text[40];
    if (c.slot < 0) std::snprintf(text, sizeof(text), "%g", c.literal);
    else std::snprintf(text, sizeof(text), "cb%d[%u].%c", c.slot, c.at / 4u, "xyzw"[c.at % 4u]);
    return text;
}

// (v0.9.553) the first few of each outcome to the log
void LogCutout(int why, const ListState& s, const RtProbeGpu::Cutout& out, std::uint64_t rawTexture, int stage,
               const RtCutout::Constant* constant, const ConstantTrace* t) {
    if (why < 0 || why >= kCutReasons || g_cutSamples[why].fetch_add(1, std::memory_order_relaxed) >= 3) return;
    const auto& p = s.pipeline;
    const RtCutout::Program& c = p.cutout;
    const D3D12_VERTEX_BUFFER_VIEW& v = s.vb[p.texcoordSlot & 3];
    std::string text;
    char line[640];
    std::snprintf(line, sizeof(line),
                  "RtProbe: cutout %s: t%u.%c at TEXCOORD%u.%c%c (layout: %s, DXGI %u, slot %u, +%u; view stride %u, "
                  "size %u); scale (%s, %s) + (%s, %s), tiling (%s, %s) + (%s, %s), threshold %s; texture descriptor "
                  "0x%llX",
                  kCutFailNames[why], c.texture, "xyzw"[c.channel & 3], c.texcoord, "xyzw"[c.component[0] & 3],
                  "xyzw"[c.component[1] & 3], p.texcoordFound ? "found" : "not found", p.texcoordFormat,
                  p.texcoordSlot, p.texcoordOffset, v.StrideInBytes, v.SizeInBytes,
                  DescribeConstant(c.vsScale[0]).c_str(), DescribeConstant(c.vsScale[1]).c_str(),
                  DescribeConstant(c.vsOffset[0]).c_str(), DescribeConstant(c.vsOffset[1]).c_str(),
                  DescribeConstant(c.psTiling[0]).c_str(), DescribeConstant(c.psTiling[1]).c_str(),
                  DescribeConstant(c.psOffset[0]).c_str(), DescribeConstant(c.psOffset[1]).c_str(),
                  DescribeConstant(c.threshold).c_str(), static_cast<unsigned long long>(rawTexture));
    text = line;
    if (constant && t) {
        std::snprintf(line, sizeof(line),
                      "; the failing constant %s (stage %d): buffer %p, size %u, data %p, resource %p +0x%llX, step %d",
                      DescribeConstant(*constant).c_str(), stage, t->cb, t->size, t->data, t->pool,
                      static_cast<unsigned long long>(t->offset), t->step);
        text += line;
    }
    if (why == kCutOk) {
        std::snprintf(line, sizeof(line), "; values: scale (%g, %g) + (%g, %g), threshold %g", out.scale[0], out.scale[1],
                      out.offset[0], out.offset[1], out.threshold);
        text += line;
    }
    if (why == kCutNoTexcoord) {   // (v0.9.554)
        text += "; its layout: ";
        text += p.layoutSummary;
    }
    Log::Write(text.c_str());
}

// (v0.9.552) A draw's alpha test resolved: its UV stream (at the draw's base vertex), texture and constants;
// (v0.9.553) the step that failed (kCutOk: resolved)
int ResolveCutout(const ListState& s, INT baseVertex, RtProbeGpu::Cutout& out) {
    const auto& p = s.pipeline;
    const RtCutout::Program& c = p.cutout;
    if (c.result != RtCutout::kFollowed || baseVertex < 0) return kCutNoTexcoord;
    HookTimer timer(kPartCutout);   // (v0.9.557)
    std::uint64_t raw = 0;
    if (!p.texcoordFound) {
        LogCutout(kCutNoTexcoord, s, out, raw, -1, nullptr, nullptr);
        return kCutNoTexcoord;
    }
    UINT bytes = 0;
    const int kind = RtProbeGpu::CutoutUvKind(static_cast<DXGI_FORMAT>(p.texcoordFormat), bytes);
    const D3D12_VERTEX_BUFFER_VIEW& v = s.vb[p.texcoordSlot & 3];
    if (kind < 0) {
        LogCutout(kCutUvFormat, s, out, raw, -1, nullptr, nullptr);
        return kCutUvFormat;
    }
    if (!v.BufferLocation || !v.StrideInBytes) {
        LogCutout(kCutUvView, s, out, raw, -1, nullptr, nullptr);
        return kCutUvView;
    }
    out.uvKind = static_cast<UINT>(kind);
    out.uvStride = v.StrideInBytes;
    for (int j = 0; j < 2; ++j) {
        out.uvAt[j] = p.texcoordOffset + c.component[j] * bytes;
        if (out.uvAt[j] + bytes > v.StrideInBytes) {
            LogCutout(kCutUvStride, s, out, raw, -1, nullptr, nullptr);
            return kCutUvStride;
        }
    }
    out.uvs = v.BufferLocation + static_cast<UINT64>(baseVertex) * v.StrideInBytes;
    out.texture = PixelTexture(t_engineContext, c.texture, &raw);
    if (!out.texture) {
        LogCutout(kCutTexture, s, out, raw, -1, nullptr, nullptr);
        return kCutTexture;
    }
    out.channel = c.channel;
    ConstantTrace t;
    for (int j = 0; j < 2; ++j) {
        float vsScale = 1.0f, vsOffset = 0.0f, tiling = 1.0f, offset = 0.0f;
        const struct {
            const RtCutout::Constant& constant;
            unsigned stage;
            float& value;
            int why;
        } reads[4] = {{c.vsScale[j], kStageVertex, vsScale, kCutVsScale},
                      {c.vsOffset[j], kStageVertex, vsOffset, kCutVsOffset},
                      {c.psTiling[j], kStagePixel, tiling, kCutTiling},
                      {c.psOffset[j], kStagePixel, offset, kCutOffset}};
        for (const auto& r : reads) {
            if (!ReadConstant(t_engineContext, r.stage, r.constant, r.value, t)) {
                LogCutout(r.why, s, out, raw, static_cast<int>(r.stage), &r.constant, &t);
                return r.why;
            }
        }
        out.scale[j] = vsScale * tiling;
        out.offset[j] = vsOffset * tiling + offset;
    }
    if (!ReadConstant(t_engineContext, kStagePixel, c.threshold, out.threshold, t)) {
        LogCutout(kCutThreshold, s, out, raw, static_cast<int>(kStagePixel), &c.threshold, &t);
        return kCutThreshold;
    }
    LogCutout(kCutOk, s, out, raw, -1, nullptr, nullptr);
    return kCutOk;
}

// (v0.9.552) Counts a depth pass's alpha test (followed and resolved, not resolved, not followed); (v0.9.553) the
// failures by step
void CountCutout(const ListState& s, int why) {
    const std::uint8_t result = s.pipeline.cutout.result;
    if (result == RtCutout::kFollowed) {
        (why == kCutOk ? g_cutTaken : g_cutUnresolved).fetch_add(1, std::memory_order_relaxed);
        if (why > kCutOk && why < kCutReasons) g_cutFail[why].fetch_add(1, std::memory_order_relaxed);
    } else if (result == RtCutout::kUnfollowed) {
        g_cutUnfollowed.fetch_add(1, std::memory_order_relaxed);
    }
}

// (v0.9.545) A skinned draw: its streams, its index range and its palette, once a frame per mesh and palette.
// (v0.9.550) An instanced one: a skinned mesh an instance, each with its palette (instance i's at i x entries in the cb;
// SV_InstanceID does not count the start instance). depthWrite false: kept apart (g_skinnedLater), its counters not
// counted (the draw's other passes count).
void CaptureSkinned(const ListState& s, UINT indexCount, UINT instances, UINT startIndex, INT baseVertex, bool depthWrite) {
    HookTimer timer(kPartSkinned);   // (v0.9.557)
    const auto& p = s.pipeline;
    auto count = [depthWrite](std::atomic<std::uint32_t>& counter) {
        if (depthWrite) counter.fetch_add(1, std::memory_order_relaxed);
    };
    if (!instances) return;
    // (v0.9.546) the palette of the cb bound as the vertex shader's b1; (v0.9.550) one an instance; (v0.9.556) read in place
    std::optional<HookTimer> finding(std::in_place, kPartSkinFind);   // (v0.9.565) until the culling
    const void* cb = g_boundCbCheck && !g_directPalettes ? BoundSkinningBuffer(t_engineContext) : nullptr;
    unsigned paletteEntries = 0;
    bool missing = false;
    const std::uint8_t* direct = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS directGpu = 0;   // (v0.9.559) a dynamic buffer's palettes on the GPU
    bool knownPoses = false;   // (v0.9.566) every instance's pose found already read this frame
    const std::uint32_t poseFrame = g_presentCount.load(std::memory_order_relaxed);
    if (g_directPalettes && instances <= t_setupDrawables.size() && t_setupFrame == poseFrame) {
        const void* boundCb = nullptr;
        std::uint32_t size = 0;
        const std::uint8_t* staticData = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
        unsigned bufferEntries = 0;   // (the buffer's own count: the kept poses' must match it)
        if (BoundPaletteSource(t_engineContext, boundCb, size, staticData, gpu, bufferEntries) && boundCb == t_setupCb) {
            t_paletteHashes.resize(instances);
            t_paletteOrigins.resize(static_cast<std::size_t>(instances) * 3);
            unsigned entries = 0;
            knownPoses = true;
            for (UINT i = 0; i < instances && knownPoses; ++i) {
                PoseSeen pose{};
                if (!FindPose(t_setupDrawables[i], poseFrame, pose) || (i && pose.entries != entries)) {
                    knownPoses = false;
                    break;
                }
                entries = pose.entries;
                t_paletteHashes[i] = pose.key;
                std::memcpy(t_paletteOrigins.data() + static_cast<std::size_t>(i) * 3, pose.origin, sizeof(pose.origin));
            }
            if (knownPoses && entries && entries == bufferEntries && (size - 16) / (entries * 64) >= instances) {
                paletteEntries = entries;
                directGpu = gpu;
                direct = staticData ? staticData + 16 : nullptr;
                g_posesKnown.fetch_add(instances, std::memory_order_relaxed);
            } else {
                knownPoses = false;
            }
        }
    }
    if (g_directPalettes && !knownPoses) {
        unsigned held = 0;
        direct = BoundPalettes(t_engineContext, paletteEntries, held, directGpu);
        if (direct && held < instances) {
            missing = true;
            direct = nullptr;
        }
        if (!direct) {
            paletteEntries = 0;
            directGpu = 0;
        }
        if (direct) {
            t_paletteHashes.resize(instances);
            t_paletteOrigins.resize(static_cast<std::size_t>(instances) * 3);
            for (UINT i = 0; i < instances; ++i)
                t_paletteHashes[i] = PaletteKey(direct + static_cast<std::size_t>(i) * paletteEntries * 64, paletteEntries,
                                                t_paletteOrigins.data() + static_cast<std::size_t>(i) * 3);
            g_posesRead.fetch_add(instances, std::memory_order_relaxed);
            // (v0.9.566) kept for this frame's other passes when the drawables are known (set up into this buffer here)
            if (instances <= t_setupDrawables.size() && t_setupFrame == poseFrame &&
                BoundSkinningBuffer(t_engineContext) == t_setupCb)
                for (UINT i = 0; i < instances; ++i) {
                    PoseSeen pose{t_paletteHashes[i], {}, paletteEntries};
                    std::memcpy(pose.origin, t_paletteOrigins.data() + static_cast<std::size_t>(i) * 3, sizeof(pose.origin));
                    KeepPose(t_setupDrawables[i], poseFrame, pose);
                }
        }
    } else if (cb) {
        std::lock_guard<std::mutex> lock(g_paletteLock);
        auto it = g_palettes.find(cb);
        if (it != g_palettes.end() && it->second.entries) {
            const KeptPalette& kept = it->second;
            if (kept.hashes.size() < instances) {
                missing = true;
            } else {
                paletteEntries = kept.entries;
                const std::size_t bytes = static_cast<std::size_t>(instances) * paletteEntries * 64;
                if (t_palette.size() < bytes) t_palette.resize(bytes);
                std::memcpy(t_palette.data(), kept.bytes.data(), bytes);
                t_paletteHashes.assign(kept.hashes.begin(), kept.hashes.begin() + instances);
                t_paletteOrigins.resize(static_cast<std::size_t>(instances) * 3);   // (v0.9.559)
                for (UINT i = 0; i < instances; ++i) {
                    const float* e = reinterpret_cast<const float*>(t_palette.data() + static_cast<std::size_t>(i) * paletteEntries * 64);
                    t_paletteOrigins[i * 3] = e[3];
                    t_paletteOrigins[i * 3 + 1] = e[7];
                    t_paletteOrigins[i * 3 + 2] = e[11];
                }
            }
        }
    }
    finding.reset();   // (v0.9.565)
    if (!paletteEntries) {
        count(missing ? g_skinnedShort : g_skinnedNoPalette);
        return;
    }
    {   // (v0.9.564) while only the lamp shadows run: the instances far from every lamp left out
        if (t_near.size() < instances) t_near.resize(instances);
        unsigned kept = 0;
        for (UINT i = 0; i < instances; ++i)
            kept += (t_near[i] = NearLamps(t_paletteOrigins.data() + static_cast<std::size_t>(i) * 3) ? 1 : 0);
        if (depthWrite && kept < instances) g_skinnedFar.fetch_add(instances - kept, std::memory_order_relaxed);
        if (!kept) return;
    }
    const bool weighted = p.weightUse != 0;   // (v0.9.547: one bone when the shader reads no weight)
    const int weightCode = weighted ? RtProbeGpu::SkinFormatCode(static_cast<DXGI_FORMAT>(p.weightFormat)) : 0;
    const int indexCode = RtProbeGpu::SkinFormatCode(static_cast<DXGI_FORMAT>(p.blendIndexFormat));
    if (static_cast<DXGI_FORMAT>(p.positionFormat) != DXGI_FORMAT_R32G32B32_FLOAT || weightCode < 0 || indexCode < 0) {
        count(g_skinnedFormat);
        return;
    }
    const D3D12_VERTEX_BUFFER_VIEW& vp = s.vb[p.positionSlot & 3];
    const D3D12_VERTEX_BUFFER_VIEW& vw = s.vb[(weighted ? p.weightSlot : p.blendIndexSlot) & 3];
    const D3D12_VERTEX_BUFFER_VIEW& vi = s.vb[p.blendIndexSlot & 3];
    const D3D12_INDEX_BUFFER_VIEW& ib = s.ib;
    const UINT indexSize = ib.Format == DXGI_FORMAT_R16_UINT ? 2 : ib.Format == DXGI_FORMAT_R32_UINT ? 4 : 0;
    if (!vp.BufferLocation || !vp.StrideInBytes || !vw.BufferLocation || !vw.StrideInBytes || !vi.BufferLocation ||
        !vi.StrideInBytes || !ib.BufferLocation || !indexSize || indexCount < 3 || indexCount % 3 || baseVertex < 0 ||
        static_cast<UINT64>(startIndex + indexCount) * indexSize > ib.SizeInBytes ||
        ((vp.BufferLocation | vw.BufferLocation | vi.BufferLocation | p.positionOffset | p.weightOffset | p.blendIndexOffset |
          vp.StrideInBytes | vw.StrideInBytes | vi.StrideInBytes) & 3)) {
        count(g_noBuffers);
        return;
    }
    // the vertices every stream holds from the base vertex on (the shader reads within the views)
    auto streamVertices = [](const D3D12_VERTEX_BUFFER_VIEW& v, unsigned offset, unsigned size) -> INT64 {
        return v.SizeInBytes < offset + size ? 0 : static_cast<INT64>((v.SizeInBytes - offset - size) / v.StrideInBytes) + 1;
    };
    // (v0.9.549) each element at its own format's size (0.9.545..548 took 16 bytes for the weights and indices: one or two
    // vertices short at a mesh's end for R8G8B8A8_UNORM (4 bytes); a triangle using them read past the mesh's posed
    // vertices into the next mesh's: the long triangles from one character to another)
    auto elementBytes = [](int code) -> unsigned { return code == 0 ? 4u : code == 3 ? 16u : 8u; };
    const INT64 total = std::min({streamVertices(vp, p.positionOffset, 12),
                                  weighted ? streamVertices(vw, p.weightOffset, elementBytes(weightCode)) : INT64_MAX,
                                  streamVertices(vi, p.blendIndexOffset, elementBytes(indexCode))});
    if (baseVertex >= total || total - baseVertex > kMaxSkinnedVertices) {
        count(g_noBuffers);
        return;
    }
    SkinnedDraw d;
    d.kind = !depthWrite ? 7 : instances > 1 ? 6 : p.weightUse ? 3 : 4;   // (v0.9.548; v0.9.550: 6, 7)
    RtProbeGpu::SkinnedGeometry& g = d.geometry;
    g.positions = vp.BufferLocation;
    g.positionStride = vp.StrideInBytes;
    g.positionOffset = p.positionOffset;
    g.weights = vw.BufferLocation;
    g.weightStride = vw.StrideInBytes;
    g.weightOffset = weighted ? p.weightOffset : p.blendIndexOffset;
    g.weightFormat = static_cast<UINT>(weightCode);
    g.blendIndices = vi.BufferLocation;
    g.indexStride = vi.StrideInBytes;
    g.indexOffset = p.blendIndexOffset;
    g.indexFormat = static_cast<UINT>(indexCode);
    g.use = (p.weightUse & 0xF) | ((p.blendIndexUse & 0xF) << 4);   // (v0.9.547)
    g.firstVertex = static_cast<UINT>(baseVertex);
    g.vertexCount = static_cast<UINT>(total - baseVertex);
    g.indices = ib.BufferLocation + static_cast<UINT64>(startIndex) * indexSize;
    g.indexBufferFormat = ib.Format;
    g.indexCount = indexCount;
    d.paletteEntries = paletteEntries;
    const int cutWhy = ResolveCutout(s, baseVertex, d.cutout);   // (v0.9.552)
    d.cut = cutWhy == kCutOk;
    d.uncertain = !d.cut && p.cutout.result != RtCutout::kNone;   // (v0.9.570)
    if (depthWrite) CountCutout(s, cutWhy);
    const std::size_t paletteBytes = static_cast<std::size_t>(paletteEntries) * 64;
    CaptureBuffer& b = ThreadBuffer();   // (v0.9.558) this thread's
    std::lock_guard<std::mutex> lock(b.lock);
    Freshen(b);   // (v0.9.563)
    const std::uint64_t geometryKey = Hash(&g, sizeof(g));
    for (UINT i = 0; i < instances; ++i) {   // (v0.9.550) each instance with its palette
        if (!t_near[i]) continue;              // (v0.9.564) far from the lamps
        d.key = geometryKey ^ (t_paletteHashes[i] * 0x9E3779B97F4A7C15ull);
        // (v0.9.556) the palette in place (the pool's copy) or this thread's copy; copied for the meshes taken only;
        // (v0.9.559) a dynamic buffer's not copied at all: posed from its GPU address
        const std::uint8_t* palette = direct ? direct + i * paletteBytes : t_palette.data() + i * paletteBytes;
        std::memcpy(d.origin, t_paletteOrigins.data() + static_cast<std::size_t>(i) * 3, sizeof(d.origin));
        d.paletteGpu = directGpu ? directGpu + i * paletteBytes : 0;
        auto keep = [&]() -> bool {
            if (d.paletteGpu) {
                d.paletteAt = 0;
                return true;
            }
            const std::size_t at = b.palettes.size();
            b.palettes.resize(at + paletteBytes);
            if (!CopySafe(b.palettes.data() + at, palette, paletteBytes)) {
                b.palettes.resize(at);
                return false;
            }
            d.paletteAt = at;
            return true;
        };
        if (!depthWrite) {   // (v0.9.550) taken at the frame's end if no pass that writes depth gives it
            if (b.keys.count(d.key) || !b.laterKeys.insert(d.key).second) continue;
            if (!keep()) continue;
            b.later.push_back(d);
            continue;
        }
        if (!b.keys.insert(d.key).second) {
            g_skinnedRepeat.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (!keep()) continue;
        b.skinned.push_back(d);
        g_skinnedTaken.fetch_add(1, std::memory_order_relaxed);
        if (instances > 1) g_skinnedFromInstanced.fetch_add(1, std::memory_order_relaxed);
    }
}

// (v0.9.545) The palette SetupSkinningMatrices wrote: entries (the float at +0) and 64 bytes each from +0x10;
// (v0.9.550) instance i's from +0x10 + i x entries x 64 (the shader's SkinMatrices[1023] hold every instance's)
unsigned ReadPalette(const void* cb, unsigned instance, std::uint8_t* out, unsigned max) noexcept {
    __try {
        const auto* data = *reinterpret_cast<const std::uint8_t* const*>(static_cast<const std::uint8_t*>(cb) + kCbData);
        if (!data) return 0;
        const float count = *reinterpret_cast<const float*>(data);
        if (!(count >= 1.0f) || count > static_cast<float>(max)) return 0;
        const unsigned n = static_cast<unsigned>(count);
        if ((static_cast<std::uint64_t>(instance) + 1) * n > max) return 0;
        std::memcpy(out, data + 0x10 + static_cast<std::size_t>(instance) * n * 64, static_cast<std::size_t>(n) * 64);
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void __fastcall HookSetupSkinning(void* drawable, void* ctx, void* cb, void* previous, int instance) {
    g_originalSetupSkinning(drawable, ctx, cb, previous, instance);
    if (g_directPalettes && Running() && cb && instance >= 0 && instance < static_cast<int>(kMaxPalette)) {
        // (v0.9.566) which drawable this buffer's instance is (instance 0 starts a buffer; the others follow in turn)
        const std::uint32_t frame = g_presentCount.load(std::memory_order_relaxed);
        if (!instance || cb != t_setupCb || t_setupFrame != frame) {
            t_setupCb = instance ? nullptr : cb;
            t_setupFrame = frame;
            t_setupDrawables.clear();
        }
        if (t_setupCb == cb && t_setupDrawables.size() == static_cast<std::size_t>(instance))
            t_setupDrawables.push_back(drawable);
        else
            t_setupCb = nullptr;   // (out of turn: not used)
        return;
    }
    if (g_directPalettes || !Running() || !cb || instance < 0 ||
        instance >= static_cast<int>(kMaxPalette))   // (v0.9.556: read in place at the draw when the pool is there)
        return;
    HookTimer timer(kPartPalette);   // (v0.9.555)
    if (t_palette.size() < kMaxPalette * 64) t_palette.resize(kMaxPalette * 64);
    const unsigned entries = ReadPalette(cb, static_cast<unsigned>(instance), t_palette.data(), kMaxPalette);
    // (a 64-bit word hash: the palette changes every frame the model moves)
    std::uint64_t h = 1469598103934665603ull ^ entries;
    const auto* w = reinterpret_cast<const std::uint64_t*>(t_palette.data());
    for (std::size_t i = 0, n = static_cast<std::size_t>(entries) * 8; i < n; ++i) h = (h ^ w[i]) * 1099511628211ull;
    std::lock_guard<std::mutex> lock(g_paletteLock);   // (v0.9.546: kept by its cb)
    if (g_palettes.size() > 16384) g_palettes.clear();
    KeptPalette& kept = g_palettes[cb];
    // (v0.9.550) instance 0 starts the cb's set; the others in turn after it (else the set is not usable until the next 0)
    if (!entries || (instance && (kept.entries != entries || kept.hashes.size() != static_cast<std::size_t>(instance)))) {
        kept.entries = 0;
        kept.bytes.clear();
        kept.hashes.clear();
        return;
    }
    if (!instance) {
        kept.bytes.clear();
        kept.hashes.clear();
        kept.entries = entries;
    }
    kept.bytes.insert(kept.bytes.end(), t_palette.begin(), t_palette.begin() + static_cast<std::size_t>(entries) * 64);
    kept.hashes.push_back(h);
}

void Capture(ID3D12GraphicsCommandList* list, UINT indexCount, UINT instances, UINT startIndex, INT baseVertex,
             UINT startInstance) {
    g_seen.fetch_add(1, std::memory_order_relaxed);
    ListState& s = StateFor(list);
    const auto& p = s.pipeline;
    if (!p.valid) {
        g_noPipeline.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (g_skyCheck && SkyDraw(t_engineContext)) {
        g_sky.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (p.skinned) g_skinned.fetch_add(1, std::memory_order_relaxed);   // (v0.9.545: every skinned draw seen)
    if (p.skinned && p.eightWeights) {   // (v0.9.545)
        g_skinned8.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool listed = p.triangles && s.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    const bool patched = p.patches && s.topology == D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
    const bool posed = p.skinned && p.blendIndexUse;   // (v0.9.550)
    if (!p.depthWrite) {
        g_noDepth.fetch_add(1, std::memory_order_relaxed);
        if (p.skinned) {   // (v0.9.550) kept apart: taken at the frame's end if no pass that writes depth gives it
            g_skinnedNoDepth.fetch_add(1, std::memory_order_relaxed);
            if (posed && p.vertexShaderKnown && (listed || patched))
                CaptureSkinned(s, indexCount, instances, startIndex, baseVertex, false);
        }
        return;
    }
    if (!listed && !patched) {
        g_topology.fetch_add(1, std::memory_order_relaxed);
        if (p.skinned) g_skinnedTopology.fetch_add(1, std::memory_order_relaxed);   // (v0.9.550)
        return;
    }
    // (v0.9.544) an instanced draw of the instancing drawable being drawn: one object per instance; (v0.9.550) a skinned
    // one posed an instance at a time from its palettes (CaptureSkinned)
    const bool multi = instances != 1;
    if (multi && !posed && (!t_instanceCount || static_cast<UINT64>(startInstance) + instances > t_instanceCount)) {
        g_instanced.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto format = static_cast<DXGI_FORMAT>(p.positionFormat);
    g_formatCounts[p.positionFormat & 255].fetch_add(1, std::memory_order_relaxed);   // (v0.9.556)
    if (!RtProbeGpu::SupportedVertexFormat(format)) {
        g_format.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (p.skinned && !p.vertexShaderKnown) {   // (v0.9.547)
        g_skinnedFormat.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (posed) {   // (v0.9.545) posed on the GPU from its streams and its palette; (v0.9.547) a shader that reads no index
                   // draws with the world matrix: below, as static
        CaptureSkinned(s, indexCount, instances, startIndex, baseVertex, true);
        return;
    }
    const D3D12_VERTEX_BUFFER_VIEW& vb = s.vb[p.positionSlot & 3];
    const D3D12_INDEX_BUFFER_VIEW& ib = s.ib;
    const UINT indexSize = ib.Format == DXGI_FORMAT_R16_UINT ? 2 : ib.Format == DXGI_FORMAT_R32_UINT ? 4 : 0;
    if (!vb.BufferLocation || !vb.StrideInBytes || !ib.BufferLocation || !indexSize || indexCount < 3 || indexCount % 3 ||
        static_cast<UINT64>(startIndex + indexCount) * indexSize > ib.SizeInBytes ||
        vb.SizeInBytes < p.positionOffset + p.positionSize) {
        g_noBuffers.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const INT64 total = static_cast<INT64>((vb.SizeInBytes - p.positionOffset - p.positionSize) / vb.StrideInBytes) + 1;
    if (baseVertex >= total || baseVertex < 0) {
        g_noBuffers.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    float m[4][4];
    if (!multi && !ReadWorld(t_engineContext, m)) {
        g_noBuffers.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Draw d;
    d.geometry.vertices = vb.BufferLocation + p.positionOffset + static_cast<UINT64>(baseVertex) * vb.StrideInBytes;
    d.geometry.vertexStride = vb.StrideInBytes;
    d.geometry.vertexFormat = format;
    d.geometry.vertexCount = static_cast<UINT>(total - baseVertex);
    d.geometry.indices = ib.BufferLocation + static_cast<UINT64>(startIndex) * indexSize;
    d.geometry.indexFormat = ib.Format;
    d.geometry.indexCount = indexCount;
    d.key = Hash(&d.geometry, sizeof(d.geometry));
    d.kind = multi ? 1 : patched ? 5 : p.skinned ? 2 : 0;   // (v0.9.548)
    // (v0.9.552) its alpha test: the mesh built not opaque (its key apart from the opaque one's)
    const int cutWhy = ResolveCutout(s, baseVertex, d.cutout);
    d.cut = cutWhy == kCutOk;
    d.uncertain = !d.cut && s.pipeline.cutout.result != RtCutout::kNone;   // (v0.9.570)
    CountCutout(s, cutWhy);
    if (d.cut) {
        const UINT uvSource[4] = {d.cutout.uvStride, d.cutout.uvKind, d.cutout.uvAt[0], d.cutout.uvAt[1]};
        d.key = Hash(uvSource, sizeof(uvSource), Hash(&d.cutout.uvs, sizeof(d.cutout.uvs), d.key ^ 0xC07C07ull));
    }
    if (patched) g_tessellated.fetch_add(1, std::memory_order_relaxed);
    CaptureBuffer& b = ThreadBuffer();   // (v0.9.558) this thread's
    std::lock_guard<std::mutex> lock(b.lock);
    Freshen(b);   // (v0.9.563)
    if (b.draws.size() + (multi ? instances : 1) > kMaxDraws) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!multi) {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) d.transform[r][c] = m[c][r];
        b.draws.push_back(d);
        g_taken.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    for (UINT i = 0; i < instances; ++i) {
        const float* w = t_instanceMatrices.data() + static_cast<std::size_t>(startInstance + i) * 16;
        if (!UsableMatrix(w)) continue;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) d.transform[r][c] = w[c * 4 + r];
        b.draws.push_back(d);
    }
    g_taken.fetch_add(1, std::memory_order_relaxed);
    g_instancedTaken.fetch_add(instances, std::memory_order_relaxed);
}

void __fastcall HookEngineDraw(void* ctx, unsigned indexCount, unsigned instances, unsigned startIndex, int baseVertex,
                               unsigned startInstance) {
    if (!Running()) {
        g_originalEngineDraw(ctx, indexCount, instances, startIndex, baseVertex, startInstance);
        return;
    }
    void* previous = t_engineContext;
    t_engineContext = ctx;
    const LightSaved saved = GiveLightPass(ctx);   // (v0.9.562; v0.9.563: inside the lights' pass's drawable, put back)
    const SunSaved sunSaved = GiveSunPass(ctx);    // (v0.9.570: inside the sun's drawable)
    g_originalEngineDraw(ctx, indexCount, instances, startIndex, baseVertex, startInstance);
    RestoreSunPass(sunSaved);
    RestoreLightPass(saved);
    t_engineContext = previous;
}

// (v0.9.570) the sun's shadow pass's drawable: the probe's sun mask given to the draws inside it (GiveSunPass)
void __fastcall HookSunPassDraw(void* drawable, unsigned pass, void* other) {
    ++t_sunPassDepth;
    g_originalSunPassDraw(drawable, pass, other);
    --t_sunPassDepth;
}

// (v0.9.563) the lights' pass's drawable: the probe's mask given to the draws inside it (GiveLightPass)
void __fastcall HookLightPassDraw(void* drawable, unsigned pass, void* other) {
    ++t_lightPassDepth;
    g_originalLightPassDraw(drawable, pass, other);
    --t_lightPassDepth;
}

// (v0.9.544) the instancing drawable: its instances read for the draws inside it
void __fastcall HookInstancingDraw(void* drawable, unsigned pass, void* other) {
    if (!Running() || !drawable) {
        g_originalInstancingDraw(drawable, pass, other);
        return;
    }
    if (t_instanceMatrices.size() < kMaxInstances * 16) t_instanceMatrices.resize(kMaxInstances * 16);
    const unsigned previous = t_instanceCount;
    {
        HookTimer timer(kPartInstancing);   // (v0.9.555)
        t_instanceCount = ReadInstances(drawable, t_instanceMatrices.data(), kMaxInstances);
    }
    g_originalInstancingDraw(drawable, pass, other);
    t_instanceCount = previous;
}

void STDMETHODCALLTYPE HookDrawIndexed(ID3D12GraphicsCommandList* list, UINT indexCount, UINT instances, UINT startIndex,
                                       INT baseVertex, UINT startInstance) {
    if (t_engineContext && Running()) {
        HookTimer timer(kPartDraw);   // (v0.9.555)
        const ListState& state = StateFor(list);
        if (state.lightPass) {
            CaptureLights(t_engineContext);
            CheckLightPass(t_engineContext);   // (v0.9.562)
        }
        if (state.sunPass) {   // (v0.9.570)
            CaptureSun(t_engineContext);
            CheckSunPass(t_engineContext);
        }
        Capture(list, indexCount, instances, startIndex, baseVertex, startInstance);
    } else if (Running()) {
        if (g_lampShadows.load(std::memory_order_relaxed) && BoundLightPass(list))
            CheckLightPass(nullptr);   // (v0.9.562) not through the engine's draw: not given
        if (g_sunShadows.load(std::memory_order_relaxed) && BoundSunPass(list)) CheckSunPass(nullptr);   // (v0.9.570)
    }
    g_realDrawIndexed(list, indexCount, instances, startIndex, baseVertex, startInstance);
}

// (v0.9.562) the lights' pass's t45 / t46: the probe's pair put in the context's descriptor cache (see the top);
// (v0.9.563) inside the lights' pass's drawable only, put back after each draw
constexpr std::size_t kCacheShaders = 0x140, kCacheStage = 0x568, kCacheSrvDirty = 0x5C8, kCacheDirtyBits = 0x1C;
constexpr unsigned kMaskSlot = 45, kTableSlot = 46;
// (v0.9.563) only inside the lights' pass's drawable (see the top): put in for the draw, marked dirty; put back after
LightSaved GiveLightPass(void* ctx) noexcept {
    LightSaved saved{};
    if (!t_lightPassDepth || !ctx || !g_lampShadows.load(std::memory_order_relaxed)) return saved;
    const std::uint64_t handle = g_lightHandle.load(std::memory_order_acquire);
    if (!handle) return saved;
    const std::uint64_t increment = g_lightIncrement.load(std::memory_order_relaxed);
    __try {
        auto* cache = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(ctx) + kCtxDescriptorCache);
        if (!cache) return saved;
        const unsigned slots[2] = {kMaskSlot, kTableSlot};
        for (int k = 0; k < 2; ++k) {
            auto* at = reinterpret_cast<std::uint64_t*>(cache + kCacheSrvs + (kStagePixel * 0xAD + slots[k]) * 8);
            saved.handles[k] = *at;
            *at = handle + static_cast<std::uint64_t>(k) * increment;
            cache[kCacheSrvDirty + kStagePixel * kCacheStage + slots[k]] = 1;
        }
        *reinterpret_cast<std::uint32_t*>(cache + kCacheDirtyBits) |= 1u << kStagePixel;
        saved.cache = cache;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        saved.cache = nullptr;
    }
    return saved;
}
void RestoreLightPass(const LightSaved& saved) noexcept {
    if (!saved.cache) return;
    __try {
        const auto* nulls = *reinterpret_cast<const std::uint8_t* const*>(saved.cache + kCacheNulls);
        const std::uint64_t none = nulls ? *reinterpret_cast<const std::uint64_t*>(nulls + kNullSrv) : 0;
        const unsigned slots[2] = {kMaskSlot, kTableSlot};
        for (int k = 0; k < 2; ++k) {
            const std::uint64_t back = saved.handles[k] ? saved.handles[k] : none;
            if (!back) continue;   // (nothing to copy back: the engine's own copy stays as it is)
            *reinterpret_cast<std::uint64_t*>(saved.cache + kCacheSrvs + (kStagePixel * 0xAD + slots[k]) * 8) = back;
            saved.cache[kCacheSrvDirty + kStagePixel * kCacheStage + slots[k]] = 1;
        }
        *reinterpret_cast<std::uint32_t*>(saved.cache + kCacheDirtyBits) |= 1u << kStagePixel;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
// At a D3D12 draw of the lights' pass (the cache flushed for it): whether its table has them. 0: nothing to check (no
// pair yet), 1: given, 2: not; meta: the pixel shader's counts, why: what was missing
int LightPassGiven(void* ctx, std::uint32_t& meta, const char*& why) noexcept {
    const std::uint64_t handle = g_lightHandle.load(std::memory_order_acquire);
    meta = 0;
    why = "";
    if (!handle) return 0;
    if (!ctx) {
        why = "drawn outside the engine's draw call";
        return 2;
    }
    const std::uint64_t increment = g_lightIncrement.load(std::memory_order_relaxed);
    __try {
        auto* cache = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(ctx) + kCtxDescriptorCache);
        if (!cache) {
            why = "no descriptor cache";
            return 2;
        }
        const auto* shader = *reinterpret_cast<const std::uint8_t* const*>(cache + kCacheShaders + kStagePixel * kCacheStage);
        meta = shader ? *reinterpret_cast<const std::uint32_t*>(shader + 0x60) : 0;
        if (((meta >> 10) & 0x7F) <= kTableSlot) {
            why = "the pixel shader's texture count does not reach t46";
            return 2;
        }
        const unsigned slots[2] = {kMaskSlot, kTableSlot};
        for (int k = 0; k < 2; ++k) {
            const auto* at = reinterpret_cast<const std::uint64_t*>(cache + kCacheSrvs + (kStagePixel * 0xAD + slots[k]) * 8);
            if (*at != handle + static_cast<std::uint64_t>(k) * increment) {
                why = "t45 / t46 not the probe's";
                return 2;
            }
            if (cache[kCacheSrvDirty + kStagePixel * kCacheStage + slots[k]] != 0) {
                why = "t45 / t46 not copied (still marked dirty)";
                return 2;
            }
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "the cache could not be read";
        return 2;
    }
}
// (v0.9.570) the sun's t42: the probe's descriptor inside the sun's drawable, marked dirty; put back after the draw
constexpr unsigned kSunSlot = 42;
SunSaved GiveSunPass(void* ctx) noexcept {
    SunSaved saved{};
    if (!t_sunPassDepth || !ctx || !g_sunShadows.load(std::memory_order_relaxed)) return saved;
    const std::uint64_t handle = g_sunHandle.load(std::memory_order_acquire);
    if (!handle) return saved;
    __try {
        auto* cache = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(ctx) + kCtxDescriptorCache);
        if (!cache) return saved;
        auto* at = reinterpret_cast<std::uint64_t*>(cache + kCacheSrvs + (kStagePixel * 0xAD + kSunSlot) * 8);
        saved.handle = *at;
        *at = handle;
        cache[kCacheSrvDirty + kStagePixel * kCacheStage + kSunSlot] = 1;
        *reinterpret_cast<std::uint32_t*>(cache + kCacheDirtyBits) |= 1u << kStagePixel;
        saved.cache = cache;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        saved.cache = nullptr;
    }
    return saved;
}
void RestoreSunPass(const SunSaved& saved) noexcept {
    if (!saved.cache) return;
    __try {
        const auto* nulls = *reinterpret_cast<const std::uint8_t* const*>(saved.cache + kCacheNulls);
        const std::uint64_t none = nulls ? *reinterpret_cast<const std::uint64_t*>(nulls + kNullSrv) : 0;
        const std::uint64_t back = saved.handle ? saved.handle : none;
        if (!back) return;
        *reinterpret_cast<std::uint64_t*>(saved.cache + kCacheSrvs + (kStagePixel * 0xAD + kSunSlot) * 8) = back;
        saved.cache[kCacheSrvDirty + kStagePixel * kCacheStage + kSunSlot] = 1;
        *reinterpret_cast<std::uint32_t*>(saved.cache + kCacheDirtyBits) |= 1u << kStagePixel;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
int SunPassGiven(void* ctx, std::uint32_t& meta, const char*& why) noexcept {
    const std::uint64_t handle = g_sunHandle.load(std::memory_order_acquire);
    meta = 0;
    why = "";
    if (!handle) return 0;
    if (!ctx) {
        why = "drawn outside the engine's draw call";
        return 2;
    }
    __try {
        auto* cache = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(ctx) + kCtxDescriptorCache);
        if (!cache) {
            why = "no descriptor cache";
            return 2;
        }
        const auto* shader = *reinterpret_cast<const std::uint8_t* const*>(cache + kCacheShaders + kStagePixel * kCacheStage);
        meta = shader ? *reinterpret_cast<const std::uint32_t*>(shader + 0x60) : 0;
        if (((meta >> 10) & 0x7F) <= kSunSlot) {
            why = "the pixel shader's texture count does not reach t42";
            return 2;
        }
        if (*reinterpret_cast<const std::uint64_t*>(cache + kCacheSrvs + (kStagePixel * 0xAD + kSunSlot) * 8) != handle) {
            why = "t42 not the probe's";
            return 2;
        }
        if (cache[kCacheSrvDirty + kStagePixel * kCacheStage + kSunSlot] != 0) {
            why = "t42 not copied (still marked dirty)";
            return 2;
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "the cache could not be read";
        return 2;
    }
}
void CheckSunPass(void* ctx) {
    if (!g_sunShadows.load(std::memory_order_relaxed)) return;
    std::uint32_t meta = 0;
    const char* why = "";
    const int given = SunPassGiven(ctx, meta, why);
    if (!given) return;
    g_sunMeta.store(meta, std::memory_order_relaxed);
    if (given == 1) {
        g_sunGiven.fetch_add(1, std::memory_order_relaxed);
        if (g_sunStreak.fetch_add(1, std::memory_order_relaxed) + 1 >= 64 &&
            g_sunRead.load(std::memory_order_relaxed) == kReadWaiting) {
            int expected = kReadWaiting;
            if (g_sunRead.compare_exchange_strong(expected, kReadOn)) {
                ShadowBlend::SetRtSunShadows(true);
                Log::Writef("RtProbe: the sun's shadow pass has the probe's sun mask (64 draws in a row checked; its pixel "
                            "shader's counts 0x%08X: %u texture slots): the ray-traced sun shadows read from now", meta,
                            (meta >> 10) & 0x7F);
            }
        }
        return;
    }
    g_sunNotGiven.fetch_add(1, std::memory_order_relaxed);
    g_sunStreak.store(0, std::memory_order_relaxed);
    if (g_sunNotGivenLogs.fetch_add(1, std::memory_order_relaxed) < 8)
        Log::Writef("RtProbe: a sun shadow pass draw without the probe's sun mask: %s (its pixel shader's counts 0x%08X)",
                    why, meta);
    int expected = kReadOn;
    if (g_sunRead.compare_exchange_strong(expected, kReadStopped)) {
        ShadowBlend::SetRtSunShadows(false);
        Log::Writef("RtProbe: a sun shadow pass draw went without the probe's sun mask (%s): the ray-traced sun shadows are "
                    "no longer read, until the game is started again", why);
    }
}

void StopRtRead(const char* why) {
    int expected = kReadOn;
    if (g_rtRead.compare_exchange_strong(expected, kReadStopped)) {
        ShadowBlend::SetRtLampShadows(false);
        Log::Writef("RtProbe: a lights' pass draw went without the probe's mask (%s): the ray-traced lamp shadows are "
                    "no longer read, until the game is started again", why);
    }
}
void CheckLightPass(void* ctx) {
    if (!g_lampShadows.load(std::memory_order_relaxed)) return;
    std::uint32_t meta = 0;
    const char* why = "";
    const int given = LightPassGiven(ctx, meta, why);
    if (!given) return;
    g_lightMeta.store(meta, std::memory_order_relaxed);
    if (given == 1) {
        g_maskInjected.fetch_add(1, std::memory_order_relaxed);
        if (g_givenStreak.fetch_add(1, std::memory_order_relaxed) + 1 >= 64 &&
            g_rtRead.load(std::memory_order_relaxed) == kReadWaiting) {
            int expected = kReadWaiting;
            if (g_rtRead.compare_exchange_strong(expected, kReadOn)) {
                ShadowBlend::SetRtLampShadows(true);
                Log::Writef("RtProbe: the lights' pass has the probe's mask (64 draws in a row checked; its pixel shader's "
                            "counts 0x%08X: %u texture slots): the ray-traced lamp shadows read from now", meta,
                            (meta >> 10) & 0x7F);
            }
        }
        return;
    }
    g_lightPassUnmatched.fetch_add(1, std::memory_order_relaxed);
    g_givenStreak.store(0, std::memory_order_relaxed);
    if (g_notGivenLogs.fetch_add(1, std::memory_order_relaxed) < 8)
        Log::Writef("RtProbe: a lights' pass draw without the probe's mask: %s (its pixel shader's counts 0x%08X)", why, meta);
    StopRtRead(why);
}

// (v0.9.555) the context's non-indexed draws (the lights' pass's quad among them): its context kept for the D3D12 draw
void __fastcall HookEngineDrawInstanced(void* ctx, unsigned vertexCount, unsigned instances, unsigned startVertex,
                                        unsigned startInstance) {
    if (!Running()) {
        g_originalEngineDrawInstanced(ctx, vertexCount, instances, startVertex, startInstance);
        return;
    }
    void* previous = t_engineContext;
    t_engineContext = ctx;
    const LightSaved saved = GiveLightPass(ctx);   // (v0.9.562; v0.9.563: inside the lights' pass's drawable, put back)
    const SunSaved sunSaved = GiveSunPass(ctx);    // (v0.9.570: inside the sun's drawable)
    g_originalEngineDrawInstanced(ctx, vertexCount, instances, startVertex, startInstance);
    RestoreSunPass(sunSaved);
    RestoreLightPass(saved);
    t_engineContext = previous;
}

void STDMETHODCALLTYPE HookDrawInstanced(ID3D12GraphicsCommandList* list, UINT vertexCount, UINT instances, UINT startVertex,
                                         UINT startInstance) {
    if (t_engineContext && Running()) {
        HookTimer timer(kPartDraw);
        const ListState& state = StateFor(list);
        if (state.lightPass) {
            CaptureLights(t_engineContext);
            CheckLightPass(t_engineContext);   // (v0.9.562) did it get the mask?
        }
        if (state.sunPass) {   // (v0.9.570)
            CaptureSun(t_engineContext);
            CheckSunPass(t_engineContext);
        }
    } else if (Running()) {
        if (g_lampShadows.load(std::memory_order_relaxed) && BoundLightPass(list))
            CheckLightPass(nullptr);   // (v0.9.562) not through the engine's draw: not given
        if (g_sunShadows.load(std::memory_order_relaxed) && BoundSunPass(list)) CheckSunPass(nullptr);   // (v0.9.570)
    }
    g_realDrawInstanced(list, vertexCount, instances, startVertex, startInstance);
}

void STDMETHODCALLTYPE HookTopology(ID3D12GraphicsCommandList* list, D3D12_PRIMITIVE_TOPOLOGY topology) {
    if (Running()) StateFor(list).topology = topology;
    g_realTopology(list, topology);
}

void STDMETHODCALLTYPE HookIndexBuffer(ID3D12GraphicsCommandList* list, const D3D12_INDEX_BUFFER_VIEW* view) {
    if (Running()) StateFor(list).ib = view ? *view : D3D12_INDEX_BUFFER_VIEW{};
    g_realIndexBuffer(list, view);
}

void STDMETHODCALLTYPE HookVertexBuffers(ID3D12GraphicsCommandList* list, UINT start, UINT count,
                                         const D3D12_VERTEX_BUFFER_VIEW* views) {
    if (Running()) {
        ListState& s = StateFor(list);
        for (UINT i = 0; i < count; ++i)
            if (start + i < 4) s.vb[start + i] = views ? views[i] : D3D12_VERTEX_BUFFER_VIEW{};
    }
    g_realVertexBuffers(list, start, count, views);
}

bool PatchSlot(void** vtable, std::size_t slot, void* hook, void** original) {
    void** at = &vtable[slot];
    if (*at == hook) return true;
    DWORD old = 0;
    if (!VirtualProtect(at, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *original = *at;
    *at = hook;
    DWORD ignored = 0;
    VirtualProtect(at, sizeof(void*), old, &ignored);
    return true;
}

// ---- the GPU side, on the overlay's present
RtProbeGpu::Probe g_probe;
bool g_probeFailed = false;
ID3D12Device* g_probeDevice = nullptr;
std::uint64_t g_uavCpu = 0, g_uavGpu = 0, g_srvCpu = 0, g_srvGpu = 0;
std::atomic<std::uint64_t> g_viewTexture{0};
bool g_wasLoading = false;
char g_status[256] = "off";
ULONGLONG g_reportAt = 0;
std::uint32_t g_frames = 0;

bool CameraNow(RtProbeGpu::Camera& out) {
    Features::DrawCameraState c{};
    if (!Features::GetDrawCameraState(c) || !c.valid) return false;
    float qx = c.quaternion[0], qy = c.quaternion[1], qz = c.quaternion[2], qw = c.quaternion[3];
    const float len = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    if (!(len > 1e-5f) || !std::isfinite(len)) return false;
    qx /= len, qy /= len, qz /= len, qw /= len;
    // as the draw overlays have it (ui.cpp BuildDrawCameraBasis)
    out.origin[0] = c.position[0], out.origin[1] = c.position[1], out.origin[2] = c.position[2];
    out.right[0] = 1 - 2 * (qy * qy + qz * qz), out.right[1] = 2 * (qx * qy + qz * qw), out.right[2] = 2 * (qx * qz - qy * qw);
    out.forward[0] = 2 * (qx * qy - qz * qw), out.forward[1] = 1 - 2 * (qx * qx + qz * qz), out.forward[2] = 2 * (qy * qz + qx * qw);
    out.up[0] = 2 * (qx * qz + qy * qw), out.up[1] = 2 * (qy * qz - qx * qw), out.up[2] = 1 - 2 * (qx * qx + qy * qy);
    float fov = c.fov;
    if (fov > 3.2f) fov *= 0.01745329251994329577f;
    fov = std::clamp(fov, 0.05f, 3.05f);
    out.tanHalfFov = std::tan(fov * 0.5f);
    out.maxDistance = 200000.0f;
    out.minDistance = 10.0f;
    return true;
}

// (v0.9.560) the lamp shadow mask's frame from the lights' pass's last copy: the mapping and the three lamps nearest
void MakeShadowSetup(RtProbeGpu::ShadowSetup& out) {
    out = RtProbeGpu::ShadowSetup{};
    if (!g_shadowMask.load(std::memory_order_relaxed) && !g_lampShadows.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_lightLock);
    if (g_lights.sceneBytes < 47 * 16) return;
    const float* sc = g_lights.scene;
    std::memcpy(out.view, sc, sizeof(out.view));
    std::memcpy(out.projection, sc + 18 * 4, sizeof(out.projection));
    std::memcpy(out.extents, sc + 45 * 4, sizeof(out.extents));
    if (!(sc[46 * 4] > 0.0f && sc[46 * 4] < 16384.0f && sc[46 * 4 + 1] > 0.0f && sc[46 * 4 + 1] < 16384.0f)) return;
    out.renderWidth = static_cast<UINT>(sc[46 * 4] + 0.5f);
    out.renderHeight = static_cast<UINT>(sc[46 * 4 + 1] + 0.5f);
    const float* v = sc;
    float camera[3];
    for (int j = 0; j < 3; ++j) camera[j] = -(v[12] * v[j * 4 + 0] + v[13] * v[j * 4 + 1] + v[14] * v[j * 4 + 2]);
    struct Candidate {
        float score;
        unsigned index;
        float world[3], reach;
    };
    std::vector<Candidate> candidates;
    const unsigned count = g_lights.lightBytes / (kLightRegisters * 16);
    for (unsigned i = 0; i < count; ++i) {
        const float* r = g_lights.lights + i * kLightRegisters * 4;
        if (!(r[3] >= 0.0f && r[3] < 3.5f)) continue;   // types 0 .. 3 (omni, spot and their capsules)
        if (!(r[8] > 0.0f)) continue;
        const float reach = 1.0f / r[8];
        if (!(reach > 10.0f && reach <= 2000.0f)) continue;   // (v0.9.561: 2,000, was 5,000: no fill lights)
        if (std::max({r[4], r[5], r[6]}) <= 0.001f) continue;
        Candidate c;
        for (int j = 0; j < 3; ++j)
            c.world[j] = (r[0] - v[12]) * v[j * 4 + 0] + (r[1] - v[13]) * v[j * 4 + 1] + (r[2] - v[14]) * v[j * 4 + 2];
        const float dx = c.world[0] - camera[0], dy = c.world[1] - camera[1], dz = c.world[2] - camera[2];
        const float gap = std::sqrt(dx * dx + dy * dy + dz * dz) - reach;
        if (gap > 2500.0f) continue;
        c.score = std::max(gap, 0.0f) - reach * 0.001f;   // (the larger reach first among those around the camera)
        c.index = i;
        c.reach = reach;
        candidates.push_back(c);
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.score < b.score; });
    for (const Candidate& c : candidates) {
        if (out.lampCount >= 3) break;
        const float* r = g_lights.lights + c.index * kLightRegisters * 4;
        float* lamp = out.lamp[out.lampCount];
        lamp[0] = c.world[0];
        lamp[1] = c.world[1];
        lamp[2] = c.world[2];
        lamp[3] = c.reach;
        float* lv = out.lampView[out.lampCount];
        lv[0] = r[0];
        lv[1] = r[1];
        lv[2] = r[2];
        lv[3] = 1.0f;
        ++out.lampCount;
    }
    out.enabled = true;
    out.show = g_shadowMask.load(std::memory_order_relaxed);   // (v0.9.561)
    out.softness = std::clamp(g_lampSoftness.load(std::memory_order_relaxed), 0.0f, 3.0f);   // (v0.9.564)
    out.rays = out.softness > 0.0f ? 4u : 1u;
    out.blurRadius = out.softness > 0.0f ? 2 : 0;
    {   // (v0.9.564) for the skinned meshes' culling
        std::lock_guard<std::mutex> cull(g_cullLock);
        std::memcpy(g_cullLamps, out.lamp, sizeof(g_cullLamps));
        g_cullCount = out.lampCount;
        g_cullKnown = true;
    }
}

// (v0.9.555) the lights' pass's lights as last copied: their count and (the first reports) the first ones in full
void LogLights(std::uint32_t frames) {
    static int s_lightLogs = 0;
    LightSnapshot l;
    {
        std::lock_guard<std::mutex> lock(g_lightLock);
        l = g_lights;
        g_lights.draws = 0;
    }
    if (s_lightLogs >= 40) return;
    ++s_lightLogs;
    const unsigned count = l.lightBytes / (kLightRegisters * 16);
    const float* v = l.scene;
    // the camera from View both ways: rows (v x M: translation in row 3) and columns (M x v: translation in column 3)
    float rowCam[3], colCam[3];
    for (int j = 0; j < 3; ++j) {
        rowCam[j] = -(v[12] * v[j * 4 + 0] + v[13] * v[j * 4 + 1] + v[14] * v[j * 4 + 2]);
        colCam[j] = -(v[3] * v[0 * 4 + j] + v[7] * v[1 * 4 + j] + v[11] * v[2 * 4 + j]);
    }
    RtProbeGpu::Camera probe;
    const bool haveProbe = CameraNow(probe);
    Log::Writef("RtProbe: the lights' pass: %u draws a frame, cb12 %u bytes (%u lights read), SceneBuffer %u bytes; the camera "
                "from View: rows (%.1f, %.1f, %.1f), columns (%.1f, %.1f, %.1f); the probe's (%.1f, %.1f, %.1f)",
                l.draws / std::max<std::uint32_t>(1, frames), l.lightBufferSize, count, l.sceneBytes, rowCam[0], rowCam[1],
                rowCam[2], colCam[0], colCam[1], colCam[2], haveProbe ? probe.origin[0] : 0.0f,
                haveProbe ? probe.origin[1] : 0.0f, haveProbe ? probe.origin[2] : 0.0f);
    if (s_lightLogs > 3) return;
    for (unsigned i = 0; i < count && i < 12; ++i) {
        const float* r = l.lights + i * kLightRegisters * 4;
        float rowWorld[3], colWorld[3];
        for (int j = 0; j < 3; ++j) {   // world = (view - T) x R^T (rows), or R^T x (view - t) (columns)
            rowWorld[j] = (r[0] - v[12]) * v[j * 4 + 0] + (r[1] - v[13]) * v[j * 4 + 1] + (r[2] - v[14]) * v[j * 4 + 2];
            colWorld[j] = (r[0] - v[3]) * v[0 * 4 + j] + (r[1] - v[7]) * v[1 * 4 + j] + (r[2] - v[11]) * v[2 * 4 + j];
        }
        std::string regs;
        char item[48];
        for (unsigned k = 0; k < kLightRegisters * 4; ++k) {
            std::snprintf(item, sizeof(item), "%s%.4g", k == 0 ? "" : (k % 4 == 0 ? " | " : " "), r[k]);
            regs += item;
        }
        std::uint32_t typeBits = 0, flagBits = 0;
        std::memcpy(&typeBits, r + 3, 4);
        std::memcpy(&flagBits, r + 24, 4);
        Log::Writef("RtProbe:   light %u: .w of +0 %.4g (bits 0x%08X), +6.x bits 0x%08X; view (%.1f, %.1f, %.1f) %.0f away; "
                    "world by rows (%.1f, %.1f, %.1f), by columns (%.1f, %.1f, %.1f); registers %s",
                    i, r[3], typeBits, flagBits, r[0], r[1], r[2], std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]),
                    rowWorld[0], rowWorld[1], rowWorld[2], colWorld[0], colWorld[1], colWorld[2], regs.c_str());
    }
}

// (v0.9.561) the probe's state between the present and the shadow pass
std::mutex g_probeLock;
// (v0.9.561) the shadow pass's list, three allocators and a fence; the game's queue's ExecuteCommandLists, hooked
struct ShadowSubmit {
    ID3D12CommandAllocator* allocators[3]{};
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = nullptr;
    UINT64 value = 0, slotValue[3]{};
    unsigned next = 0;
    bool failed = false;
} g_shadowSubmit;
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
ExecuteFn g_realExecute = nullptr;
ID3D12CommandQueue* g_shadowQueue = nullptr;
void** g_queueVtable = nullptr;
std::atomic<std::uint32_t> g_shadowPasses{0}, g_shadowBusy{0};

ShadowSubmit g_sunSubmit;   // (v0.9.570) the sun's pass's own
bool EnsureShadowSubmit(ID3D12Device* device, ShadowSubmit& s = g_shadowSubmit) {
    if (s.list) return true;
    if (s.failed || !device) return false;
    bool ok = true;
    for (auto*& a : s.allocators)
        ok = ok && SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)));
    ok = ok && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocators[0], nullptr,
                                                   IID_PPV_ARGS(&s.list)));
    if (ok) s.list->Close();
    ok = ok && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence)));
    s.event = ok ? CreateEventW(nullptr, FALSE, FALSE, nullptr) : nullptr;
    if (!ok || !s.event) {
        s.failed = true;
        Log::Write("RtProbe: the lamp shadows' command list could not be made; no lamp shadows");
        return false;
    }
    return true;
}

// (v0.9.562) the lights' pass's pairs (see the top): a CPU-only heap of the framework's own, never released (the
// engine's caches keep its handles); written under g_probeLock
constexpr unsigned kLightPairs = 8;
ID3D12DescriptorHeap* g_lightHeap = nullptr;
ID3D12Device* g_lightHeapDevice = nullptr;
unsigned g_lightPairNext = 0;
std::uint32_t g_lightGeneration = 0;   // the probe's mask generation the published pair has
void WriteNullPair(unsigned pair) {
    const SIZE_T increment = g_lightIncrement.load(std::memory_order_relaxed);
    const SIZE_T at = g_lightHeap->GetCPUDescriptorHandleForHeapStart().ptr + pair * 2 * increment;
    D3D12_SHADER_RESOURCE_VIEW_DESC null{};
    null.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    null.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    null.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    null.Texture2D.MipLevels = 1;
    g_lightHeapDevice->CreateShaderResourceView(nullptr, &null, D3D12_CPU_DESCRIPTOR_HANDLE{at});
    g_lightHeapDevice->CreateShaderResourceView(nullptr, &null, D3D12_CPU_DESCRIPTOR_HANDLE{at + increment});
}
bool EnsureLightHeap(ID3D12Device* device) {
    if (g_lightHeap && g_lightHeapDevice == device) return true;
    if (g_lightHeap) {   // (another device: the old one's contexts are gone)
        g_lightHandle.store(0, std::memory_order_release);
        g_lightHeap->Release();
        g_lightHeap = nullptr;
        g_lightHeapDevice = nullptr;
    }
    if (!device) return false;
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 2 * kLightPairs;
    if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_lightHeap))) || !g_lightHeap) {
        g_lightHeap = nullptr;
        return false;
    }
    g_lightHeapDevice = device;
    g_lightIncrement.store(device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV),
                           std::memory_order_relaxed);
    for (unsigned i = 0; i < kLightPairs; ++i) WriteNullPair(i);
    return true;
}
// after a shadow pass: the mask's and table's descriptors in a new pair when the mask was made again
void PublishLightPass() {
    if (!EnsureLightHeap(g_probeDevice)) return;
    const std::uint32_t generation = g_probe.MaskGeneration();
    if (g_lightHandle.load(std::memory_order_relaxed) && generation == g_lightGeneration) return;
    const unsigned pair = 1 + g_lightPairNext++ % (kLightPairs - 1);
    const SIZE_T increment = g_lightIncrement.load(std::memory_order_relaxed);
    const SIZE_T at = g_lightHeap->GetCPUDescriptorHandleForHeapStart().ptr + pair * 2 * increment;
    if (!g_probe.WriteLightPassDescriptors(D3D12_CPU_DESCRIPTOR_HANDLE{at}, D3D12_CPU_DESCRIPTOR_HANDLE{at + increment})) return;
    g_lightGeneration = generation;
    g_lightEpoch.fetch_add(1, std::memory_order_acq_rel);
    g_lightHandle.store(at, std::memory_order_release);
}
// the probe stopping (its resources released): the pairs nulled, none published; the patched pass no longer reads them
void UnpublishLightPass() {
    ShadowBlend::SetRtLampShadows(false);
    int expected = kReadOn;
    g_rtRead.compare_exchange_strong(expected, kReadWaiting);
    g_givenStreak.store(0, std::memory_order_relaxed);
    g_lightHandle.store(0, std::memory_order_release);
    if (g_lightHeap)
        for (unsigned i = 0; i < kLightPairs; ++i) WriteNullPair(i);
    g_lightEpoch.fetch_add(1, std::memory_order_acq_rel);
    g_lightGeneration = 0;
}

// (v0.9.570) the sun's descriptor: a CPU-only heap of 8 (0 a null one; 1 .. 7 in turn, a new one when the sun's mask is
// made again), never released; written under g_probeLock
constexpr unsigned kSunHeapSlots = 8;
ID3D12DescriptorHeap* g_sunHeap = nullptr;
ID3D12Device* g_sunHeapDevice = nullptr;
UINT g_sunIncrement = 0;
unsigned g_sunNext = 0;
std::uint32_t g_sunGeneration = 0;
void WriteNullSun(unsigned slot) {
    D3D12_SHADER_RESOURCE_VIEW_DESC null{};
    null.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    null.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    null.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    null.Texture2D.MipLevels = 1;
    g_sunHeapDevice->CreateShaderResourceView(
        nullptr, &null, D3D12_CPU_DESCRIPTOR_HANDLE{g_sunHeap->GetCPUDescriptorHandleForHeapStart().ptr +
                                                    static_cast<SIZE_T>(slot) * g_sunIncrement});
}
bool EnsureSunHeap(ID3D12Device* device) {
    if (g_sunHeap && g_sunHeapDevice == device) return true;
    if (g_sunHeap) {
        g_sunHandle.store(0, std::memory_order_release);
        g_sunHeap->Release();
        g_sunHeap = nullptr;
        g_sunHeapDevice = nullptr;
    }
    if (!device) return false;
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = kSunHeapSlots;
    if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_sunHeap))) || !g_sunHeap) {
        g_sunHeap = nullptr;
        return false;
    }
    g_sunHeapDevice = device;
    g_sunIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (unsigned i = 0; i < kSunHeapSlots; ++i) WriteNullSun(i);
    return true;
}
void PublishSun() {
    if (!EnsureSunHeap(g_probeDevice)) return;
    const std::uint32_t generation = g_probe.SunGeneration();
    if (g_sunHandle.load(std::memory_order_relaxed) && generation == g_sunGeneration) return;
    const unsigned slot = 1 + g_sunNext++ % (kSunHeapSlots - 1);
    const SIZE_T at = g_sunHeap->GetCPUDescriptorHandleForHeapStart().ptr + static_cast<SIZE_T>(slot) * g_sunIncrement;
    if (!g_probe.WriteSunPassDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE{at})) return;
    g_sunGeneration = generation;
    g_sunHandle.store(at, std::memory_order_release);
}
void UnpublishSun() {
    ShadowBlend::SetRtSunShadows(false);
    int expected = kReadOn;
    g_sunRead.compare_exchange_strong(expected, kReadWaiting);
    g_sunStreak.store(0, std::memory_order_relaxed);
    g_sunHandle.store(0, std::memory_order_release);
    if (g_sunHeap)
        for (unsigned i = 0; i < kSunHeapSlots; ++i) WriteNullSun(i);
    g_sunGeneration = 0;
}
// the sun's frame: the camera from the sun pass's SceneBuffer; the sun's axis from WorldToPSSM0's third column (the map's
// depth; row vectors: map = world x M), turned to the side with z up (the sun or moon above); 0.5 degree x the softness
RtProbeGpu::ShadowSetup g_lastSun;
float g_sunRaw[3] = {};   // (for the log) the column as read
void MakeSunSetup(RtProbeGpu::ShadowSetup& out) {
    out = RtProbeGpu::ShadowSetup{};
    if (!g_sunShadows.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_sunLock);
    if (g_sun.sceneBytes < 58 * 16) return;
    const float* sc = g_sun.scene;
    std::memcpy(out.view, sc, sizeof(out.view));
    std::memcpy(out.projection, sc + 18 * 4, sizeof(out.projection));
    std::memcpy(out.extents, sc + 45 * 4, sizeof(out.extents));
    if (!(sc[46 * 4] > 0.0f && sc[46 * 4] < 16384.0f && sc[46 * 4 + 1] > 0.0f && sc[46 * 4 + 1] < 16384.0f)) return;
    out.renderWidth = static_cast<UINT>(sc[46 * 4] + 0.5f);
    out.renderHeight = static_cast<UINT>(sc[46 * 4 + 1] + 0.5f);
    float d[3] = {sc[54 * 4 + 2], sc[55 * 4 + 2], sc[56 * 4 + 2]};
    std::memcpy(g_sunRaw, d, sizeof(d));
    const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(length > 1.0e-12f) || !std::isfinite(length)) return;
    const float sign = d[2] < 0.0f ? -1.0f : 1.0f;
    for (int j = 0; j < 3; ++j) out.sun[j] = d[j] / length * sign;
    const float softness = std::clamp(g_lampSoftness.load(std::memory_order_relaxed), 0.0f, 3.0f);
    out.sun[3] = std::tan(0.5f * 0.0174532925f * softness);
    out.softness = softness;
    out.rays = softness > 0.0f ? 4u : 1u;
    out.blurRadius = softness > 0.0f ? 2 : 0;
    out.sunDistance = 15000.0f;   // (v0.9.571; was 25,000)
    out.enabled = true;
    {   // (v0.9.571) the characters' range for the sun: its surfaces' + 10 m (a character's shadow lies near it)
        float camera[3];
        const float* v = sc;
        for (int j = 0; j < 3; ++j) camera[j] = -(v[12] * v[j * 4 + 0] + v[13] * v[j * 4 + 1] + v[14] * v[j * 4 + 2]);
        std::lock_guard<std::mutex> cull(g_cullLock);
        std::memcpy(g_cullCamera, camera, sizeof(camera));
        g_cullCamera[3] = out.sunDistance + 1000.0f;
    }
}
void SubmitSunPass(ID3D12CommandQueue* queue) {
    std::unique_lock<std::mutex> lock(g_probeLock, std::try_to_lock);
    if (!lock.owns_lock()) {
        g_sunBusy.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!g_probe.Ready() || !g_probeDevice || !EnsureShadowSubmit(g_probeDevice, g_sunSubmit)) return;
    RtProbeGpu::ShadowSetup setup;
    MakeSunSetup(setup);
    if (!setup.enabled) return;
    g_lastSun = setup;
    g_probe.SetSunShadows(setup);
    ShadowSubmit& s = g_sunSubmit;
    const unsigned slot = s.next++ % 3;
    if (s.fence->GetCompletedValue() < s.slotValue[slot]) {
        s.fence->SetEventOnCompletion(s.slotValue[slot], s.event);
        WaitForSingleObject(s.event, 200);
        if (s.fence->GetCompletedValue() < s.slotValue[slot]) return;
    }
    if (FAILED(s.allocators[slot]->Reset()) || FAILED(s.list->Reset(s.allocators[slot], nullptr))) return;
    const bool ok = g_probe.RecordSunPass(s.list, slot);
    s.list->Close();
    if (!ok) return;
    ID3D12CommandList* lists[] = {s.list};
    g_realExecute(queue, 1, lists);
    queue->Signal(s.fence, ++s.value);
    s.slotValue[slot] = s.value;
    PublishSun();
    g_sunPasses.fetch_add(1, std::memory_order_relaxed);
}

void SubmitShadowPass(ID3D12CommandQueue* queue) {
    std::unique_lock<std::mutex> lock(g_probeLock, std::try_to_lock);
    if (!lock.owns_lock()) {
        g_shadowBusy.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!g_probe.Ready() || !g_probeDevice || !EnsureShadowSubmit(g_probeDevice)) return;
    RtProbeGpu::ShadowSetup setup;
    MakeShadowSetup(setup);
    if (!setup.enabled || !setup.lampCount) return;
    g_probe.SetShadows(setup);
    ShadowSubmit& s = g_shadowSubmit;
    const unsigned slot = s.next++ % 3;
    if (s.fence->GetCompletedValue() < s.slotValue[slot]) {
        s.fence->SetEventOnCompletion(s.slotValue[slot], s.event);
        WaitForSingleObject(s.event, 200);
        if (s.fence->GetCompletedValue() < s.slotValue[slot]) return;
    }
    if (FAILED(s.allocators[slot]->Reset()) || FAILED(s.list->Reset(s.allocators[slot], nullptr))) return;
    const bool ok = g_probe.RecordShadowPass(s.list, slot);
    s.list->Close();
    if (!ok) return;
    ID3D12CommandList* lists[] = {s.list};
    g_realExecute(queue, 1, lists);
    queue->Signal(s.fence, ++s.value);
    s.slotValue[slot] = s.value;
    PublishLightPass();   // (v0.9.562)
    g_shadowPasses.fetch_add(1, std::memory_order_relaxed);
}

void STDMETHODCALLTYPE HookExecute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    if (queue == g_shadowQueue && Running() && g_sunShadows.load(std::memory_order_relaxed)) {   // (v0.9.570)
        const std::uint64_t seq = g_sunSeq.load(std::memory_order_acquire);
        if (seq != g_sunSeqDone.load(std::memory_order_relaxed)) {
            g_sunSeqDone.store(seq, std::memory_order_relaxed);
            SubmitSunPass(queue);
        }
    }
    if (queue == g_shadowQueue && Running() &&
        (g_lampShadows.load(std::memory_order_relaxed) || g_shadowMask.load(std::memory_order_relaxed))) {
        const std::uint64_t seq = g_lightSeq.load(std::memory_order_acquire);
        if (seq != g_shadowSeqDone.load(std::memory_order_relaxed)) {
            g_shadowSeqDone.store(seq, std::memory_order_relaxed);
            SubmitShadowPass(queue);
        }
    }
    g_realExecute(queue, count, lists);
}

void Report(ULONGLONG now) {
    const auto& s = g_probe.LastStats();
    const std::uint32_t seen = g_seen.exchange(0), taken = g_taken.exchange(0), skinned = g_skinned.exchange(0),
                        instanced = g_instanced.exchange(0), format = g_format.exchange(0), topology = g_topology.exchange(0),
                        noDepth = g_noDepth.exchange(0), noPipeline = g_noPipeline.exchange(0),
                        noBuffers = g_noBuffers.exchange(0), dropped = g_dropped.exchange(0),
                        instancedTaken = g_instancedTaken.exchange(0), sky = g_sky.exchange(0),
                        tessellated = g_tessellated.exchange(0), skinnedTaken = g_skinnedTaken.exchange(0),
                        skinned8 = g_skinned8.exchange(0), skinnedNoPalette = g_skinnedNoPalette.exchange(0),
                        skinnedFormat = g_skinnedFormat.exchange(0), skinnedRepeat = g_skinnedRepeat.exchange(0),
                        skinnedFromInstanced = g_skinnedFromInstanced.exchange(0), skinnedShort = g_skinnedShort.exchange(0),
                        skinnedTopology = g_skinnedTopology.exchange(0), skinnedNoDepth = g_skinnedNoDepth.exchange(0),
                        skinnedOnlyNoDepth = g_skinnedOnlyNoDepth.exchange(0),   // (v0.9.550)
                        cutTaken = g_cutTaken.exchange(0), cutUnresolved = g_cutUnresolved.exchange(0),
                        cutUnfollowed = g_cutUnfollowed.exchange(0);   // (v0.9.552)
    static std::uint64_t s_builtBefore = 0, s_evictedBefore = 0;
    const std::uint64_t builtNow = s.builtTotal - s_builtBefore, evictedNow = s.evictedTotal - s_evictedBefore;
    s_builtBefore = s.builtTotal;
    s_evictedBefore = s.evictedTotal;
    const std::uint32_t frames = std::max<std::uint32_t>(1, g_frames);
    g_frames = 0;
    std::snprintf(g_status, sizeof(g_status),
                  "%u objects traced (%u skinned, %u waiting, %u cut out), %u meshes kept, %.0f MB; a frame's draws: %u "
                  "seen, %u taken (%u instances), %u skinned taken, %u skinned not",
                  s.instances, s.skinned, s.instancesWaiting, s.cutouts, s.meshes, (s.memory + s.skinnedMemory) / 1048576.0,
                  seen / frames, taken / frames, instancedTaken / frames, skinnedTaken / frames,
                  (skinned8 + skinnedNoPalette + skinnedFormat + skinnedShort) / frames);
    std::string formats;
    for (unsigned i = 0; i < 256; ++i) {   // (v0.9.556)
        const std::uint32_t n = g_formatCounts[i].exchange(0, std::memory_order_relaxed);
        if (!n) continue;
        char item[40];
        std::snprintf(item, sizeof(item), "%s%u x%u", formats.empty() ? "" : ", ", i, n / frames);
        formats += item;
    }
    static int s_logs = 0;
    if (s_logs++ < 60)
        Log::Writef("RtProbe: %u frames: per frame %u engine draws seen, %u taken (%u of them tessellated; %u instances "
                    "from instanced ones); %u skinned seen (below); not taken: %u instanced (not the instancing drawable's), %u sky, %u "
                    "position format, %u topology, %u no depth write, %u no pipeline record, %u buffers / matrix not usable, "
                    "%u over the cap; position formats (DXGI, per frame) [%s]. Traced: %u objects, %u waiting for their "
                    "mesh; %u meshes kept (%u failed), %llu built and %llu let go in these frames, %.1f MB",
                    frames, seen / frames, taken / frames, tessellated / frames, instancedTaken / frames, skinned / frames,
                    instanced / frames, sky / frames, format / frames, topology / frames, noDepth / frames,
                    noPipeline / frames, noBuffers / frames, dropped, formats.c_str(), s.instances, s.instancesWaiting,
                    s.meshes, s.failed, static_cast<unsigned long long>(builtNow), static_cast<unsigned long long>(evictedNow),
                    s.memory / 1048576.0);
    if (s_logs <= 60)
        Log::Writef("RtProbe:   skinned per frame: %u taken (%u of them instances of instanced draws; %u more passes of "
                    "them), %u drawn only without depth writes taken; not taken: %u with 8 weights, %u without a palette, %u "
                    "instanced without every instance's palette, %u weight / index format, %u topology; %u without depth "
                    "writes (passes of the above); last frame %u posed (%u vertices, %u over the limits, nearest first: the "
                    "farthest posed %.0f from the camera), %.1f MB",
                    skinnedTaken / frames, skinnedFromInstanced / frames, skinnedRepeat / frames, skinnedOnlyNoDepth / frames,
                    skinned8 / frames, skinnedNoPalette / frames, skinnedShort / frames, skinnedFormat / frames,
                    skinnedTopology / frames, skinnedNoDepth / frames, s.skinned, s.skinnedVertices, s.skinnedDropped,
                    g_skinnedFarthest, s.skinnedMemory / 1048576.0);
    if (s_logs <= 60)
        Log::Writef("RtProbe:   the last frame's objects by kind: %u static, %u instanced, %u skinned layouts drawn rigid "
                    "(world matrix), %u four-weight skinned, %u one-bone skinned, %u tessellated, %u skinned instances of "
                    "instanced draws, %u skinned drawn only without depth writes", g_kindCounts[0], g_kindCounts[1],
                    g_kindCounts[2], g_kindCounts[3], g_kindCounts[4], g_kindCounts[5], g_kindCounts[6], g_kindCounts[7]);
    if (s_logs <= 60)   // (v0.9.552)
        Log::Writef("RtProbe:   cutouts per frame: %u draws with their alpha test taken, %u not resolved (texture / constants), "
                    "%u with an alpha test not followed; last frame %u instances traced with it (%u records, %u textures), "
                    "%u traced opaque (no room), %u meshes' UVs gathered, the UVs %.1f MB",
                    cutTaken / frames, cutUnresolved / frames, cutUnfollowed / frames, s.cutouts, s.cutoutRecords,
                    s.cutoutTextures, s.cutoutRefused, s.uvGathered, s.uvMemory / 1048576.0);
    if (s_logs <= 60) {   // (v0.9.553)
        std::string reasons;
        for (int i = 1; i < kCutReasons; ++i) {
            const std::uint32_t n = g_cutFail[i].exchange(0);
            if (!n) continue;
            char item[64];
            std::snprintf(item, sizeof(item), "%s%u %s", reasons.empty() ? "" : ", ", n / frames, kCutFailNames[i]);
            reasons += item;
        }
        Log::Writef("RtProbe:   cutouts not resolved per frame, by step: %s", reasons.empty() ? "none" : reasons.c_str());
    }
    {   // (v0.9.555) the time
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        const double toMs = 1000.0 / static_cast<double>(frequency.QuadPart);
        double part[kParts];
        for (int i = 0; i < kParts; ++i) part[i] = g_cpuPartTicks[i].exchange(0) * toMs / frames;   // (v0.9.557)
        const double frame = g_cpuFrameTicks * toMs / frames, record = g_cpuRecordTicks * toMs / frames;
        g_cpuFrameTicks = g_cpuRecordTicks = 0;
        if (s_logs <= 60)
            Log::Writef("RtProbe:   time per frame: CPU in the hooks (all threads) %.2f ms: SetPipelineState %.2f, the draws "
                        "%.2f (of it the skinned capture %.2f (finding and keying its palettes %.2f), the cutouts %.2f), the "
                        "instancing reads %.2f, the palette hook "
                        "%.2f; at the present %.2f ms (%.2f of it recording); GPU %.2f ms (static builds %.2f, skinned %.2f, "
                        "UV gathers %.2f, top level %.2f, view %.2f)",
                        part[kPartPipeline] + part[kPartDraw] + part[kPartInstancing] + part[kPartPalette],
                        part[kPartPipeline], part[kPartDraw], part[kPartSkinned], part[kPartSkinFind], part[kPartCutout],
                        part[kPartInstancing],
                        part[kPartPalette], frame, record, s.gpuMs[6], s.gpuMs[0], s.gpuMs[1], s.gpuMs[2], s.gpuMs[3],
                        s.gpuMs[5]);
        if (s_logs <= 60 && g_lastShadow.enabled) {   // (v0.9.560)
            const RtProbeGpu::ShadowSetup& m = g_lastShadow;
            float scene85[4] = {};
            {
                std::lock_guard<std::mutex> lock(g_lightLock);
                std::memcpy(scene85, g_lights.scene + 85 * 4, sizeof(scene85));
            }
            std::string lamps;
            for (UINT k = 0; k < m.lampCount; ++k) {
                char item[96];
                std::snprintf(item, sizeof(item), "%s(%.0f, %.0f, %.0f) reach %.0f", k ? ", " : "", m.lamp[k][0], m.lamp[k][1],
                              m.lamp[k][2], m.lamp[k][3]);
                lamps += item;
            }
            Log::Writef("RtProbe:   lamp shadow mask: %u x %u (render %u x %u), %u lamps [%s], %.2f ms on the GPU; "
                        "SceneBuffer [18] (%.5g, %.5g, %.5g, %.5g), [45] (%.5g, %.5g, %.5g, %.5g), [85] (%.5g, %.5g, %.5g, %.5g)",
                        s.maskWidth, s.maskHeight, m.renderWidth, m.renderHeight, s.maskLamps, lamps.c_str(), s.shadowMs,
                        m.projection[0], m.projection[1], m.projection[2], m.projection[3], m.extents[0], m.extents[1],
                        m.extents[2], m.extents[3], scene85[0], scene85[1], scene85[2], scene85[3]);
        }
    }
    if (s_logs <= 60)   // (v0.9.558)
        Log::Writef("RtProbe:   the dynamic pool's lookups past the threads' kept blocks: %u a frame (%u blocks walked); %zu "
                    "capturing threads", g_poolMisses.exchange(0) / frames, g_poolWalked.exchange(0) / frames,
                    [] {
                        std::lock_guard<std::mutex> lock(g_buffersLock);
                        return g_buffers.size();
                    }());
    if (s_logs <= 60 && (g_lampShadows.load() || g_shadowMask.load()))   // (v0.9.561)
        Log::Writef("RtProbe:   lamp shadows: %u passes before the lights' pass, %u skipped (the present busy); the lights' "
                    "pass given the mask %u times, not %u times (its pixel shader's counts 0x%08X); %s; softness %.2f (%s); "
                    "%u skinned instances a frame left out (farther than reach + %.0f cm from every lamp, or with the "
                    "sun's shadows on, than 160 m from the camera); skinned "
                    "instances' palettes read %u a frame, found already read %u a frame",
                    g_shadowPasses.exchange(0), g_shadowBusy.exchange(0), g_maskInjected.exchange(0),
                    g_lightPassUnmatched.exchange(0), g_lightMeta.load(), LampShadowsState(), g_lampSoftness.load(),
                    g_lampSoftness.load() > 0.0f ? "4 rays a lamp, blurred 2 texels each way" : "one ray a lamp, not blurred",
                    g_skinnedFar.exchange(0) / frames, kCullMargin, g_posesRead.exchange(0) / frames,
                    g_posesKnown.exchange(0) / frames);
    if (s_logs <= 60 && g_sunShadows.load()) {   // (v0.9.570)
        const RtProbeGpu::ShadowSetup& m = g_lastSun;
        Log::Writef("RtProbe:   sun shadows: %u passes before the sun's shadow pass, %u skipped (the present busy); the sun "
                    "pass given the mask %u times, not %u times (its pixel shader's counts 0x%08X); %s; mask %u x %u, %.2f "
                    "ms on the GPU; toward the sun (%.3f, %.3f, %.3f) (WorldToPSSM0's column (%.4g, %.4g, %.4g)), cone "
                    "%.2f degree, %u rays; the sun pass's draws %u a frame; %u uncertain instances (alpha tests not "
                    "resolved: the sun's rays through them give the texel up to the cascades)",
                    g_sunPasses.exchange(0), g_sunBusy.exchange(0), g_sunGiven.exchange(0), g_sunNotGiven.exchange(0),
                    g_sunMeta.load(), SunShadowsState(), s.sunWidth, s.sunHeight, s.sunMs, m.sun[0], m.sun[1], m.sun[2],
                    g_sunRaw[0], g_sunRaw[1], g_sunRaw[2], std::atan(m.sun[3]) * 57.2957795f, m.rays,
                    [frames] {
                        std::lock_guard<std::mutex> lock(g_sunLock);
                        const std::uint32_t draws = g_sun.draws;
                        g_sun.draws = 0;
                        return frames ? draws / frames : 0u;
                    }(), s.uncertain);
    }
    LogLights(frames);   // (v0.9.555)
    g_reportAt = now;
}
}  // namespace

bool Install() {
    if (!GameBuild::IsSupportedRetail()) return false;
    if (!GameBuild::VerifyBytes(kRvaEngineDrawIndexed, kEngineDrawIndexedSig, sizeof(kEngineDrawIndexedSig)) ||
        !GameBuild::VerifyBytes(kRvaSetWorldMatrix, kSetWorldMatrixSig, sizeof(kSetWorldMatrixSig))) {
        Log::Write("RtProbe: the engine's draw or world matrix does not match this build; no ray tracing probe");
        return false;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(GameBuild::Base() + kRvaEngineDrawIndexed), reinterpret_cast<void*>(&HookEngineDraw),
                      reinterpret_cast<void**>(&g_originalEngineDraw)) != MH_OK) {
        Log::Write("RtProbe: the engine's draw hook could not be created; no ray tracing probe");
        return false;
    }
    // (v0.9.555) the context's non-indexed draws (optional: the lamps' data not read without it)
    if (!GameBuild::VerifyBytes(kRvaEngineDrawInstanced, kEngineDrawInstancedSig, sizeof(kEngineDrawInstancedSig)) ||
        MH_CreateHook(reinterpret_cast<void*>(GameBuild::Base() + kRvaEngineDrawInstanced),
                      reinterpret_cast<void*>(&HookEngineDrawInstanced),
                      reinterpret_cast<void**>(&g_originalEngineDrawInstanced)) != MH_OK)
        Log::Write("RtProbe: the engine's non-indexed draw does not match this build; the lamps' data not read");
    // (v0.9.563) the lights' pass's drawable (without it the lamp shadows are never read: no draw is given the mask)
    if (!GameBuild::VerifyBytes(kRvaLightPassDraw, kLightPassDrawSig, sizeof(kLightPassDrawSig)) ||
        MH_CreateHook(reinterpret_cast<void*>(GameBuild::Base() + kRvaLightPassDraw),
                      reinterpret_cast<void*>(&HookLightPassDraw), reinterpret_cast<void**>(&g_originalLightPassDraw)) != MH_OK)
        Log::Write("RtProbe: TiledLightingApplyDrawable::Draw does not match this build; the ray-traced lamp shadows are "
                   "not given to the lights' pass");
    // (v0.9.570) the sun's shadow pass's drawable (without it the sun mask is never read)
    if (!GameBuild::VerifyBytes(kRvaSunPassDraw, kSunPassDrawSig, sizeof(kSunPassDrawSig)) ||
        MH_CreateHook(reinterpret_cast<void*>(GameBuild::Base() + kRvaSunPassDraw),
                      reinterpret_cast<void*>(&HookSunPassDraw), reinterpret_cast<void**>(&g_originalSunPassDraw)) != MH_OK)
        Log::Write("RtProbe: DirectionalShadowDrawable::Draw does not match this build; the ray-traced sun shadows are "
                   "not given to the sun's shadow pass");
    // (v0.9.544) the instancing drawable and the sky's mode (each optional)
    if (GameBuild::VerifyBytes(kRvaInstancingDraw, kInstancingDrawSig, sizeof(kInstancingDrawSig)) &&
        GameBuild::VerifyBytes(kRvaInstancingCount, kInstancingCountSig, sizeof(kInstancingCountSig)) &&
        GameBuild::VerifyBytes(kRvaInstancingList, kInstancingListSig, sizeof(kInstancingListSig)) &&
        GameBuild::VerifyBytes(kRvaInstancingMatrix, kInstancingMatrixSig, sizeof(kInstancingMatrixSig))) {
        if (MH_CreateHook(reinterpret_cast<void*>(GameBuild::Base() + kRvaInstancingDraw),
                          reinterpret_cast<void*>(&HookInstancingDraw), reinterpret_cast<void**>(&g_originalInstancingDraw)) != MH_OK)
            Log::Write("RtProbe: the instancing drawable's hook could not be created; instanced objects stay out");
    } else {
        Log::Write("RtProbe: ModelInstancingDrawable::Draw does not match this build; instanced objects stay out");
    }
    g_skyCheck = GameBuild::VerifyBytes(kRvaSetSkySphereMode, kSetSkySphereModeSig, sizeof(kSetSkySphereModeSig));
    // (v0.9.545) the skinning palettes (optional: skinned meshes stay out without it); (v0.9.546) the context's bound
    // constant buffers' place checked too
    g_boundCbCheck = GameBuild::VerifyBytes(kRvaContextSetCb, kContextSetCbSig, sizeof(kContextSetCbSig));
    // (v0.9.554) the dynamic pool (the cutouts' dynamic constants: their thresholds)
    g_poolCheck = GameBuild::VerifyBytes(kRvaDynamicCbMap, kDynamicCbMapSig, sizeof(kDynamicCbMapSig));
    g_directPalettes = g_poolCheck && g_boundCbCheck;   // (v0.9.556)
    if (!g_poolCheck)
        Log::Write("RtProbe: PCDX12DynamicConstantBuffer::Map does not match this build; dynamic constants (the cutouts' "
                   "thresholds) not read");
    if (!g_boundCbCheck || !GameBuild::VerifyBytes(kRvaSetupSkinning, kSetupSkinningSig, sizeof(kSetupSkinningSig)) ||
        MH_CreateHook(reinterpret_cast<void*>(GameBuild::Base() + kRvaSetupSkinning), reinterpret_cast<void*>(&HookSetupSkinning),
                      reinterpret_cast<void**>(&g_originalSetupSkinning)) != MH_OK)
        Log::Write("RtProbe: ModelDrawable::SetupSkinningMatrices or the context's constant buffers do not match, or the hook "
                   "could not be made; skinned meshes stay out");
    if (!g_skyCheck) Log::Write("RtProbe: SetSkySphereMode does not match this build; the sky's geometry is not told apart");
    g_installed = true;
    return true;
}

void AttachCommandList(ID3D12GraphicsCommandList* list) {
    if (!g_installed || !list) return;
    void** vtable = *reinterpret_cast<void***>(list);
    if (!vtable) return;
    std::lock_guard<std::mutex> lock(g_attachLock);
    if (g_hookedVtable == vtable) return;
    if (g_hookedVtable) {
        static bool s_logged = false;
        if (!s_logged) Log::Write("RtProbe: a command list of another kind (its draws are not seen)");
        s_logged = true;
        return;
    }
    const bool ok = PatchSlot(vtable, kSlotDrawIndexed, reinterpret_cast<void*>(&HookDrawIndexed), reinterpret_cast<void**>(&g_realDrawIndexed)) &&
                    PatchSlot(vtable, kSlotTopology, reinterpret_cast<void*>(&HookTopology), reinterpret_cast<void**>(&g_realTopology)) &&
                    PatchSlot(vtable, kSlotIndexBuffer, reinterpret_cast<void*>(&HookIndexBuffer), reinterpret_cast<void**>(&g_realIndexBuffer)) &&
                    PatchSlot(vtable, kSlotVertexBuffers, reinterpret_cast<void*>(&HookVertexBuffers),
                              reinterpret_cast<void**>(&g_realVertexBuffers)) &&
                    PatchSlot(vtable, kSlotDrawInstanced, reinterpret_cast<void*>(&HookDrawInstanced),
                              reinterpret_cast<void**>(&g_realDrawInstanced));   // (v0.9.555)
    g_hookedVtable = vtable;
    Log::Write(ok ? "RtProbe: ready (the command lists' draws and input buffers seen; off until chosen)"
                  : "RtProbe: the command lists' vtable could not be hooked");
}

namespace {
// (v0.9.557) what was read of a pipeline, kept per thread (its record asked for once, not at every bind)
struct PipelineSeen {
    ID3D12PipelineState* pipeline = nullptr;
    std::uint32_t generation = 0;
    bool lightPass = false;
    bool sunPass = false;   // (v0.9.570)
    EightWeightPatch::PipelineGeometry geometry{};
};
thread_local PipelineSeen t_pipelinesSeen[64];
}  // namespace

void OnSetPipelineState(ID3D12GraphicsCommandList* list, ID3D12PipelineState* gamePipeline) {
    if (!Running() || !list) return;
    HookTimer timer(kPartPipeline);   // (v0.9.556)
    ListState& s = StateFor(list);
    if (!gamePipeline) {
        s.pipeline = {};
        s.lightPass = false;
        s.sunPass = false;
        return;
    }
    // (v0.9.557) this thread's copy while the pipeline is the same one (no game pipeline gone since it was read)
    const std::uint32_t generation = EightWeightPatch::PipelineGeneration();
    PipelineSeen& seen = t_pipelinesSeen[(reinterpret_cast<std::uintptr_t>(gamePipeline) >> 6) & 63];
    if (seen.pipeline != gamePipeline || seen.generation != generation) {
        seen.pipeline = gamePipeline;
        seen.generation = generation;
        seen.geometry = {};
        EightWeightPatch::PipelineGeometryOf(gamePipeline, seen.geometry);
        seen.lightPass = EightWeightPatch::IsLightPassPipeline(gamePipeline);   // (v0.9.555)
        seen.sunPass = EightWeightPatch::IsSunPassPipeline(gamePipeline);       // (v0.9.570)
    }
    s.pipeline = seen.geometry;
    s.lightPass = seen.lightPass;
    s.sunPass = seen.sunPass;
}

void RecordFrame(ID3D12Device* device, ID3D12GraphicsCommandList* list, unsigned frameIndex, unsigned width, unsigned height) {
    const int mode = g_mode.load(std::memory_order_relaxed);
    const bool running = Running();   // (v0.9.561: the view, or the lamp shadows)
    static bool s_releasedWhenOff = true;
    if (!mode) g_viewTexture.store(0);
    if (!running || !g_installed || !g_hookedVtable || g_probeFailed || !device || !list) {
        if (!running && g_probe.Ready()) {   // (off: the meshes let go once, their memory freed some frames later)
            if (!s_releasedWhenOff) g_probe.ResetMeshes();
            s_releasedWhenOff = true;
            g_probe.Idle();
        }
        return;
    }
    s_releasedWhenOff = false;
    std::unique_lock<std::mutex> probeLock(g_probeLock);   // (v0.9.561) the shadow pass shares the probe
    if (!g_queueVtable && (g_lampShadows.load() || g_shadowMask.load() || g_sunShadows.load())) {   // (v0.9.561) the game's queue's submissions
        if (ID3D12CommandQueue* queue = Dx12Overlay::Queue()) {
            void** vtable = *reinterpret_cast<void***>(queue);
            if (vtable && PatchSlot(vtable, 10, reinterpret_cast<void*>(&HookExecute), reinterpret_cast<void**>(&g_realExecute))) {
                g_shadowQueue = queue;
                g_queueVtable = vtable;
                Log::Write("RtProbe: the game's queue's submissions hooked (the lamp shadows' pass goes before them)");
            }
        }
    }
    if (g_probe.Ready() && g_probeDevice != device) {
        g_probe.Shutdown();
        g_uavCpu = g_uavGpu = g_srvCpu = g_srvGpu = 0;
    }
    if (!g_probe.Ready()) {
        std::string error;
        if (!g_probe.Initialize(device, error)) {
            g_probeFailed = true;
            std::snprintf(g_status, sizeof(g_status), "unavailable: %s", error.c_str());
            Log::Writef("RtProbe: %s", g_status);
            return;
        }
        g_probeDevice = device;
        g_probe.SetTimestampFrequency(Dx12Overlay::TimestampFrequency());   // (v0.9.555)
        Log::Write("RtProbe: DirectX Raytracing tier 1.1 there; the probe's shader and buffers made");
    }
    if (!g_uavCpu && !(Dx12Overlay::AllocateDescriptor(&g_uavCpu, &g_uavGpu) && Dx12Overlay::AllocateDescriptor(&g_srvCpu, &g_srvGpu))) {
        g_uavCpu = 0;
        std::snprintf(g_status, sizeof(g_status), "no descriptor left in the overlay's heap");
        return;
    }
    LARGE_INTEGER frameStart;   // (v0.9.555) this function's time from here
    QueryPerformanceCounter(&frameStart);
    g_frameNumber.fetch_add(1, std::memory_order_relaxed);
    // (v0.9.558) the threads' captures: each buffer's lists swapped with its spares (emptied here last frame), read from
    // the spares; the skinned meshes deduplicated across threads, those drawn only without depth writes added after
    std::vector<CaptureBuffer*> buffers;
    {
        std::lock_guard<std::mutex> lock(g_buffersLock);
        buffers = g_buffers;
    }
    for (CaptureBuffer* b : buffers) {
        b->spareDraws.clear();
        b->spareSkinned.clear();
        b->spareLater.clear();
        b->sparePalettes.clear();
        std::lock_guard<std::mutex> lock(b->lock);
        b->draws.swap(b->spareDraws);
        b->skinned.swap(b->spareSkinned);
        b->later.swap(b->spareLater);
        b->palettes.swap(b->sparePalettes);
        b->keys.clear();
        b->laterKeys.clear();
    }
    std::vector<SkinnedDraw> skinnedDraws;   // (v0.9.545)
    {
        std::unordered_set<std::uint64_t> keys;
        std::uint32_t repeats = 0, later = 0;
        for (CaptureBuffer* b : buffers)
            for (SkinnedDraw k : b->spareSkinned) {
                if (!keys.insert(k.key).second) {
                    ++repeats;
                    continue;
                }
                k.palette = k.paletteGpu ? nullptr : b->sparePalettes.data() + k.paletteAt;   // (v0.9.559)
                skinnedDraws.push_back(k);
            }
        for (CaptureBuffer* b : buffers)
            for (SkinnedDraw k : b->spareLater) {
                if (!keys.insert(k.key).second) continue;
                k.palette = k.paletteGpu ? nullptr : b->sparePalettes.data() + k.paletteAt;   // (v0.9.559)
                skinnedDraws.push_back(k);
                ++later;
            }
        g_skinnedOnlyNoDepth.fetch_add(later, std::memory_order_relaxed);
        g_skinnedRepeat.fetch_add(repeats, std::memory_order_relaxed);   // (taken by two threads: counted once)
        g_skinnedTaken.fetch_sub(std::min(repeats, g_skinnedTaken.load(std::memory_order_relaxed)), std::memory_order_relaxed);
    }
    const bool loading = Features::LoadingProfilerSessionActive();
    if (loading || g_wasLoading) {
        if (loading && !g_wasLoading) Log::Write("RtProbe: a load: the meshes built so far are let go (their buffers may move)");
        if (loading) g_probe.ResetMeshes();
        g_wasLoading = loading;
        if (loading) return;
    }
    // (v0.9.551) the camera first: the skinned meshes go nearest it first
    RtProbeGpu::Camera camera;
    camera.kindView = g_kindView.load(std::memory_order_relaxed) ? 1.0f : 0.0f;   // (v0.9.548)
    if (!CameraNow(camera)) {
        std::snprintf(g_status, sizeof(g_status), "no camera yet");
        return;
    }
    // each mesh and transform once (the shadow passes draw what the camera's pass does)
    g_probe.BeginFrame();
    std::unordered_set<std::uint64_t> seen;
    std::size_t drawCount = 0;
    for (CaptureBuffer* b : buffers) drawCount += b->spareDraws.size();
    seen.reserve(drawCount);
    std::uint32_t kinds[8] = {};   // (v0.9.548; v0.9.550: 8)
    for (CaptureBuffer* b : buffers)   // (v0.9.558) each thread's
        for (const Draw& d : b->spareDraws) {
            const std::uint64_t id = Hash(d.transform, sizeof(d.transform), d.key);
            if (!seen.insert(id).second) continue;
            ++kinds[d.kind & 7];
            g_probe.AddInstance(d.key, d.geometry, d.transform,
                                (static_cast<std::uint32_t>(id ^ (id >> 24)) & 0x1FFFFF) | (std::uint32_t(d.kind) << 21),
                                d.cut ? &d.cutout : nullptr, d.uncertain);
        }
    // (v0.9.545) the skinned meshes; (v0.9.551) nearest the camera first, by the palette's first entry's translation (its
    // columns' fourth components: where it puts the model's origin), the GPU side taking them up to its limits
    std::vector<std::pair<float, std::size_t>> order;
    order.reserve(skinnedDraws.size());
    for (std::size_t i = 0; i < skinnedDraws.size(); ++i) {
        const float* o = skinnedDraws[i].origin;   // (v0.9.559: kept at the capture)
        const float dx = o[0] - camera.origin[0], dy = o[1] - camera.origin[1], dz = o[2] - camera.origin[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        order.emplace_back(std::isfinite(d2) ? d2 : FLT_MAX, i);
    }
    std::sort(order.begin(), order.end());
    float farthest = -1.0f;
    for (const auto& item : order) {
        const SkinnedDraw& k = skinnedDraws[item.second];
        if (!g_probe.AddSkinned(k.geometry, k.palette, k.paletteEntries,
                                (static_cast<std::uint32_t>(Hash(&k.geometry, sizeof(k.geometry))) & 0x1FFFFF) |
                                    (std::uint32_t(k.kind) << 21),
                                k.cut ? &k.cutout : nullptr, k.paletteGpu, k.uncertain))
            continue;
        ++kinds[k.kind & 7];   // (v0.9.551: those posed)
        if (item.first < FLT_MAX) farthest = item.first;
    }
    g_skinnedFarthest = farthest >= 0.0f ? std::sqrt(farthest) : -1.0f;
    std::memcpy(g_kindCounts, kinds, sizeof(kinds));   // (v0.9.548)
    D3D12_CPU_DESCRIPTOR_HANDLE uavCpu{static_cast<SIZE_T>(g_uavCpu)}, srvCpu{static_cast<SIZE_T>(g_srvCpu)};
    D3D12_GPU_DESCRIPTOR_HANDLE uavGpu{g_uavGpu};
    MakeShadowSetup(g_lastShadow);   // (v0.9.560)
    g_probe.SetShadows(g_lastShadow);
    LARGE_INTEGER recordStart, recordEnd;   // (v0.9.555)
    QueryPerformanceCounter(&recordStart);
    const bool ok = g_probe.Record(list, frameIndex, width, height, camera, uavCpu, uavGpu, srvCpu, mode != 0);
    QueryPerformanceCounter(&recordEnd);
    g_cpuFrameTicks += static_cast<std::uint64_t>(recordEnd.QuadPart - frameStart.QuadPart);
    g_cpuRecordTicks += static_cast<std::uint64_t>(recordEnd.QuadPart - recordStart.QuadPart);
    g_viewTexture.store(ok && mode ? g_srvGpu : 0);
    ++g_frames;
    const ULONGLONG now = GetTickCount64();
    if (!g_reportAt) g_reportAt = now;
    if (now - g_reportAt >= 5000) Report(now);
}

void OnOverlayReleased() {
    {   // (v0.9.562) before the mask and table are released
        std::lock_guard<std::mutex> lock(g_probeLock);
        UnpublishLightPass();
        UnpublishSun();   // (v0.9.570)
    }
    if (g_probe.Ready()) g_probe.Shutdown();   // (the overlay waited for the GPU)
    g_uavCpu = g_uavGpu = g_srvCpu = g_srvGpu = 0;
    g_viewTexture.store(0);
}

void TickGameThread() {
    if (Running()) Features::SnapshotDrawCameraNow();
}

int Mode() { return g_mode.load(); }
void SetMode(int mode) {
    mode = std::clamp(mode, 0, 3);
    const int was = g_mode.exchange(mode);
    if (was == mode) return;
    if (!mode && !g_lampShadows.load() && !g_sunShadows.load()) {   // (v0.9.561: kept while the lamp shadows run; v0.9.570 or the sun's)
        {   // (v0.9.558) every thread's buffer
            std::lock_guard<std::mutex> buffersLock(g_buffersLock);
            for (CaptureBuffer* b : g_buffers) {
                std::lock_guard<std::mutex> lock(b->lock);
                b->draws.clear();
                b->skinned.clear();
                b->later.clear();
                b->palettes.clear();
                b->keys.clear();
                b->laterKeys.clear();
            }
        }
        std::lock_guard<std::mutex> paletteLock(g_paletteLock);   // (v0.9.546)
        g_palettes.clear();
        std::snprintf(g_status, sizeof(g_status), "off");
    }
    Log::Writef("RtProbe: %s", mode == 0 ? "off" : mode == 1 ? "on (full screen)" : mode == 2 ? "on (split screen)"
                                                                                            : "on (over the game's image)");
}
bool Active() { return g_mode.load() != 0 && g_installed; }
bool KindView() { return g_kindView.load(); }   // (v0.9.548)
void SetKindView(bool on) { g_kindView.store(on); }
bool ShadowMask() { return g_shadowMask.load(); }   // (v0.9.560)
void SetShadowMask(bool on) { g_shadowMask.store(on); }
bool LampShadows() { return g_lampShadows.load(); }   // (v0.9.561)
float LampSoftness() { return g_lampSoftness.load(); }   // (v0.9.564)
void SetLampSoftness(float softness) { g_lampSoftness.store(std::clamp(softness, 0.0f, 3.0f)); }
void SetLampShadows(bool on) {
    if (!on) {   // (v0.9.562) the patched pass stops reading at once; on: once the check holds (CheckLightPass)
        ShadowBlend::SetRtLampShadows(false);
        int expected = kReadOn;
        g_rtRead.compare_exchange_strong(expected, kReadWaiting);
        g_givenStreak.store(0, std::memory_order_relaxed);
    }
    if (g_lampShadows.exchange(on) != on)
        Log::Writef("RtProbe: ray-traced lamp shadows %s%s", on ? "on" : "off",
                    on && g_rtRead.load() == kReadStopped ? " (but stopped earlier: a lights' pass draw went without the "
                                                            "mask; start the game again)" : "");
}
void OnPresent() { g_presentCount.fetch_add(1, std::memory_order_relaxed); }   // (v0.9.563)
bool NeedsFrames() {
    return g_installed && (g_lampShadows.load(std::memory_order_relaxed) || g_sunShadows.load(std::memory_order_relaxed));
}
// (v0.9.602) RecordFrame has work in the overlay's frame: the probe running (its view, the lamp or sun shadows), or its
// meshes still to let go of once it is off
bool RecordsFrames() { return g_installed && (Running() || g_probe.Ready()); }
bool SunShadows() { return g_sunShadows.load(); }   // (v0.9.570)
void SetSunShadows(bool on) {
    {   // (v0.9.571) the characters' range for the sun not known until its next pass
        std::lock_guard<std::mutex> cull(g_cullLock);
        g_cullCamera[3] = 0.0f;
    }
    if (!on) {   // the patched pass stops reading at once; on: once the check holds (CheckSunPass)
        ShadowBlend::SetRtSunShadows(false);
        int expected = kReadOn;
        g_sunRead.compare_exchange_strong(expected, kReadWaiting);
        g_sunStreak.store(0, std::memory_order_relaxed);
    }
    if (g_sunShadows.exchange(on) != on)
        Log::Writef("RtProbe: ray-traced sun shadows %s%s", on ? "on" : "off",
                    on && g_sunRead.load() == kReadStopped ? " (but stopped earlier: a sun shadow pass draw went without "
                                                             "the mask; start the game again)" : "");
}
const char* SunShadowsState() {
    if (!g_sunShadows.load()) return "off";
    switch (g_sunRead.load()) {
        case kReadOn: return "on: the sun's shadow pass reads the mask";
        case kReadStopped: return "stopped: a sun shadow pass draw went without the mask (start the game again)";
        default: return g_sunHandle.load() ? "checking the sun's shadow pass" : "waiting for the first sun mask";
    }
}
const char* LampShadowsState() {   // (v0.9.562)
    if (!g_lampShadows.load()) return "off";
    switch (g_rtRead.load()) {
        case kReadOn: return "on: the lights' pass reads the mask";
        case kReadStopped: return "stopped: a lights' pass draw went without the mask (start the game again)";
        default: return g_lightHandle.load() ? "checking the lights' pass" : "waiting for the first mask";
    }
}
std::uint64_t ViewTexture() { return g_viewTexture.load(); }
const char* Status() { return g_status; }
}  // namespace RtProbe
