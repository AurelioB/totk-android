"""Curated companion decorations rendered from the player's Common archive.

Every exported entry records its layout/pane or texture provenance. Nine-slice
insets are source pixels, after alpha cropping. Artwork is precolored unless
mask=true (white RGB, original alpha; use color when drawing). References are
static authored states, not claims of runtime-equivalent screenshots: no Link
model, live item captures, language text, blur/capture shaders or dynamic lists.
"""
import json
from pathlib import Path

from PIL import Image, ImageDraw

from bflyt_render import Renderer


APPROXIMATIONS = [
    'Static offline 2-D layout rendering; text panes are skipped.',
    'Framebuffer/capture textures are skipped, including BNTX placeholder bytes; no Link model or live item/map captures.',
    'TEV-less material colors are lerp(black,white,texture) multiplied by vertex RGBA; multiple texture maps use multiplication, not arbitrary TEV/indirect shaders.',
    'No framebuffer blur, procedural effects, scissor clipping, automatic alignment, extended user data or runtime pane bindings.',
    'X/Y pane rotations are orthographically projected; Z transforms, origins, parent offsets, visibility and inherited alpha are applied.',
    'One-material corner frames mirror the upper-left artwork; horizontal cap windows and four/eight-material perimeter frames are composited separately.',
    'Default layout visibility is retained in *-default references; *-state references apply the explicitly listed authored animation samples.',
]


def rgba(color):
    return '#' + ''.join(f'{c:02X}' for c in color)


def extract_decor(archive, out, texture_loader, save_webp, review_dir=None):
    renderer = Renderer(archive, texture_loader)
    decor, sources, images = {}, {}, {}

    def material(layout, name):
        return next(m for m in renderer.layout(layout).materials if m.name == name)

    def emit(name, image, source, usage, nine=None, mask=False, color=None, tile=False):
        if image is None or image.getchannel('A').getbbox() is None:
            raise ValueError(f'Empty companion decor: {name} ({source})')
        if mask:
            image = Image.merge('RGBA', (Image.new('L', image.size, 255),)*3 + (image.getchannel('A'),))
        path = 'decor/' + name + '.webp'
        save_webp(image, out / path, 95)
        entry = dict(path=path, w=image.width, h=image.height)
        if nine is not None: entry['nine'] = list(nine)
        if mask: entry['mask'] = True
        if color is not None: entry['color'] = rgba(color)
        if tile: entry['tile'] = True
        decor[name], sources[name], images[name] = entry, dict(source=source, usage=usage), image
        return image

    def panes(name, layout, selected, usage='stretch', nine=None, crop=True, size=None, view=None, force=False, **kwargs):
        selected = (selected,) if isinstance(selected, str) else tuple(selected)
        image = None
        for pane in selected:
            layer = renderer.render(layout, pane, size=size, view=view, force=force)
            if image is None: image = layer
            else: image.alpha_composite(layer)
        if crop:
            box = image.getchannel('A').getbbox()
            if box: image = image.crop(box)
        return emit(name, image, layout + '/' + '+'.join(selected), usage, nine, **kwargs)

    def texture(name, source, **kwargs):
        return emit(name, renderer.texture(source), 'timg/__Combined.bntx/' + source, **kwargs)

    palette = {
        'text': rgba(material('PaPouchContentInfo_00', 'T_Caption_00').white),
        'textDim': rgba(material('PaAppMapCursor_00', 'T_Hint_00').white[:3] +
                        (renderer.layout('PaAppMapCursor_00').panes['T_Hint_00'].alpha,)),
        'accent': rgba(material('PaSplitLine_00', 'P_SplitLine_00').white),
        'accentDim': rgba(material('PauseMenuOverlay_00', 'P_DecoL_00').white),
        'panel': rgba(material('PaPouchActionWindow_00', 'P_Base_00').white),
        'gold': rgba(material('PaIconBtnLight_01', 'W_FrameBornOut_00LT').white),
        'cursor': rgba(material('PaIconBtnLight_01', 'P_Decide_00').white),
        'background': rgba(material('PaPouchActionWindow_00', 'P_Base_00').white),
        'equipped': rgba(material('PaIconBtnLight_01', 'W_FrameBornOut_00LT').white),
        'stamina': rgba(material('PaStaminaGauge_00', 'P_GaugeMain_00').white),
        'energy': rgba(material('PaEnergyGauge_00', 'P_Main_04').white),
    }

    wash = material('PaPouchActionWindow_00', 'P_Base_00').white[:3]
    wash_alpha = renderer.layout('PaPouchActionWindow_00').panes['P_Base_00'].alpha
    image = Image.new('RGBA', (1920,1080), wash + (wash_alpha,))
    emit('background', image, 'PaPouchActionWindow_00/P_Base_00 material+alpha', 'stretch; authored dark warm translucent wash')
    texture('background_pattern', 'AppMenuBGStar_00^s', usage='tile; subtle authored star/noise mask',
            tile=True, mask=True, color=material('PauseMenuOverlay_00', 'P_DecoL_00').white)
    # The authored window is 936px tall inside the 1080px layout. Export its
    # actual bounds, not transparent 74/70px canvas margins: stretching those
    # margins as full-view art creates hard header/footer alpha discontinuities.
    panes('vignette', 'AppMenuBG_00', 'W_Shadow_00', 'stretch full view; alpha-cropped soft black edge fade')
    panes('deco_left', 'PauseMenuOverlay_00', 'P_DecoL_00', 'uniform scale; gold ripples anchored bottom-left')
    panes('deco_right', 'PauseMenuOverlay_00', 'P_DecoR_00', 'uniform scale; gold ripples anchored bottom-right')
    panes('header_line', 'PaSplitLine_00', 'P_SplitLine_00', 'stretch horizontally; warm cream title rule')
    panes('divider', 'PaSplitLine_00', 'P_SplitLine_00', 'stretch horizontally; cream 2px rule')
    panes('footer', 'PaSplitLine_00', 'P_SplitLine_00', 'stretch horizontally; thin cream footer rule (not the raw solid footer prototype)')
    panes('tab', 'PaCommonBtnThin_00', 'P_Base_00', 'nine-slice; clean dark unselected Pouch plate', nine=(8,8,8,8))
    panes('tab_selected', 'PaCommonBtnThin_00', ('P_Base_00','W_Frame_00'), 'nine-slice; dark/gold selected Pouch button', nine=(8,8,8,8))
    panes('slot', 'PaIconBtnLight_01', ('P_Base_00', 'W_Frame_00'), 'nine-slice; empty inventory cell backing', nine=(8,8,8,8))
    panes('slot_selected', 'PaIconBtnLight_01', 'W_FrameBornOut_00', 'nine-slice; gold cursor overlay with transparent center', nine=(8,8,8,8))
    panes('info_panel', 'PaPouchActionWindow_00', 'P_Base_01', 'stretch; dark item information backing (capture-free material)')
    panes('window', 'PaPouchActionWindow_00', ('P_Base_01','W_Frame_01'), 'nine-slice; dark/gold generic window', nine=(8,8,8,8))
    panes('title_plate', 'PaCommonBtnThin_00', ('P_Base_00','W_Frame_00'), 'nine-slice; clean dark Pouch plate with thin gold border', nine=(8,8,8,8))
    panes('scrollbar_track', 'PaScrollBar_01', ('P_Line_00','P_Base_00'), 'stretch vertically; inventory scrollbar outline and black track')
    panes('scrollbar_thumb', 'PaScrollBar_01', 'P_ScrollBar_00', 'stretch vertically; cream inventory thumb')
    panes('map_frame', 'AppMenuOverlay_00', 'N_FrameAlbNrm_00', 'stretch; authored Purah Pad stone side frame', crop=False)
    panes('map_north', 'PaAppMapNorthMark_00', 'RootPane', 'uniform scale; north arrow and backing')
    texture('map_cursor', 'MapCursorCenter_00^s', usage='uniform scale; actual map cursor center mask in warm menu palette',
            mask=True, color=material('PaSplitLine_00','P_SplitLine_00').white)
    panes('gauge_stamina', 'PaStaminaGauge_00', 'P_GaugeMain_02', 'uniform scale; one stamina ring (1000 capacity), clip for fractional fill')
    panes('gauge_stamina_empty', 'PaStaminaGauge_00', 'P_BaseGauge_02', 'uniform scale; one empty stamina ring (1000 capacity)')
    panes('gauge_battery', 'PaEnergyGauge_00', ('P_Base_01','P_Main_04'), 'uniform scale; battery frame and green cells',
          crop=False, size=(128,88), view=(-64,44,128,88))
    panes('gauge_battery_empty', 'PaEnergyGauge_00', ('P_Base_00','P_Base_01'), 'uniform scale; empty battery frame',
          crop=False, size=(128,88), view=(-64,44,128,88))
    for key, tex in (('ground','Ground'), ('sky','Sky'), ('depths','Cave')):
        texture('layer_'+key, 'MapTabIcon_'+tex+'^s', usage='uniform scale; tintable map layer glyph',
                mask=True, color=material('PaSplitLine_00', 'P_SplitLine_00').white)
    page_icons = {}
    for key, tex in (('adventure','Nt_PauseIconCh_06^s'), ('inventory','Nt_PauseIcon_10^s'), ('map','MapTabIcon_Ground^s')):
        texture('page_'+key, tex, usage='uniform scale; tintable page glyph', mask=True, color=material('PaPouchTab_00','T_Text_00').white)
        page_icons[key] = decor['page_'+key]['path']

    if review_dir is not None:
        write_review(renderer, Path(review_dir), images, sources, palette)
    return decor, palette, page_icons, sources


def write_review(renderer, out, images, sources, palette):
    out.mkdir(parents=True, exist_ok=True)
    continuous = Image.new('RGBA', (1920,1080), (0,0,0,255))
    continuous.alpha_composite(images['background'])
    continuous.alpha_composite(images['vignette'].resize(continuous.size, Image.Resampling.LANCZOS))
    continuous.save(out/'10-continuous-warm-background.png')
    names = ('PauseMenuBG_00','PauseMenuOverlay_00','AppMenuBG_00','AppMenuOverlay_00','AppMap_00',
             'PaPouchTab_00','PaPouchSlideListBtnSet_00','PaPouchContentInfo_00','PaAppMenuTitle_00')
    for name in names:
        renderer.render(name).save(out / (name+'-default.png'))
    # These layouts require runtime bindings to populate their lists and hide
    # transient effects. Keep a raw default render alongside these state samples.
    states = {'PaIconBtn_01': [('PaIconBtn_01_Check',0),('PaIconBtn_01_UnSelect',14)]}
    renderer.render('PaPouchSlideListBtnSet_00', animations=states).save(out/'PaPouchSlideListBtnSet_00-unselected.png')
    states['PaIconBtn_01'] = [('PaIconBtn_01_Check',0),('PaIconBtn_01_Select',1)]
    renderer.render('PaPouchSlideListBtnSet_00', animations=states).save(out/'PaPouchSlideListBtnSet_00-selected.png')
    for frame in (1,2):
        renderer.render('PaPouchTab_00', animations={'PaPouchTab_00':[('PaPouchTab_00_State',frame)]}).save(out/f'PaPouchTab_00-state{frame}.png')
    background = renderer.render('PauseMenuBG_00')
    background.alpha_composite(renderer.render('Pouch_00'))
    background.alpha_composite(renderer.render('PauseMenuOverlay_00'))
    background.save(out/'Pouch_00-inventory-default.png')
    app = renderer.render('AppMenuBG_00')
    app.alpha_composite(renderer.render('AppMenuOverlay_00'))
    app.save(out/'AppMenu-composite-default.png')
    # Art-only views suppress mutually exclusive popup/effect instances that
    # the live game's bindings normally hide. They are explicitly not raw
    # defaults and do not invent item captures, text or a player model.
    inventory_hidden = (
        'Pa_RemoveAttachment_00','Pa_SortWindow_00','Pa_ActionWindow_00',
        'Pa_DungeonClearSeal_00','Pa_WarpIcon_00','Pa_HiddenScroll_00',
        'Pa_SageWill_00','Pa_SageIcon_00','Pa_ItemBoxIcon_00','Pa_SlotIcon_00',
        'N_SystemButton_00','N_DisplayPlayerStatus_00','Pa_HeartGauge_00',
        'N_ParamInvalid_00','Pa_SpIcon_00','N_BornOut_00','W_Smoke_00','W_FrameBornOut_00',
        'W_BaseCategory_00','N_InOut_01')
    inventory_states = {
        'PaPouchSlideList_01': [('PaPouchSlideList_01_CategoryChange',6)],
        'PaIconBtnLight_01': [('PaIconBtnLight_01_Check',0),('PaIconBtnLight_01_UnSelect',14)]}
    inventory = Image.new('RGBA', (1920,1080), palette['panel'])
    inventory.alpha_composite(renderer.render('PauseMenuBG_00'))
    inventory.alpha_composite(renderer.render('Pouch_00',hidden=inventory_hidden,animations=inventory_states))
    inventory.alpha_composite(renderer.render('PauseMenuOverlay_00',hidden=('P_FooterBase_00',)))
    inventory.alpha_composite(images['footer'].resize((1920,2)), (0,1010))
    inventory.save(out/'Pouch_00-inventory-art-only.png')
    inventory.save(out/'03-dark-warm-inventory-art-only.png')
    # Detail/title layouts contain mostly text and live captures; show their
    # actual art subtrees on the same panel instead of a misleading black PNG.
    for name, pane in (('PaPouchTab_00','RootPane'),('PaPouchContentInfo_00','RootPane'),
                       ('PaAppMenuTitle_00','W_Light_00')):
        panel = Image.new('RGBA',(1920,1080),palette['panel'])
        panel.alpha_composite(renderer.render(name,pane=pane))
        panel.save(out/(name+'-art-only.png'))
    # Contact sheet uses a neutral panel, so dark alpha artwork and white masks
    # remain visible. Labels and source inventory are review-only, never shipped.
    cell_w, cell_h, cols = 300, 220, 4
    sheet = Image.new('RGB', (cols*cell_w, ((len(images)+cols-1)//cols)*cell_h), (43,45,44))
    draw = ImageDraw.Draw(sheet)
    for i, (name, image) in enumerate(images.items()):
        image = image.copy()
        image.thumbnail((cell_w-20,cell_h-40), Image.Resampling.LANCZOS)
        x, y = (i%cols)*cell_w, (i//cols)*cell_h
        sheet.paste(image, (x+(cell_w-image.width)//2,y+(cell_h-40-image.height)//2), image)
        draw.text((x+10,y+cell_h-32), name, fill=(255,252,198))
    sheet.save(out/'decor-sheet.png')
    sheet.save(out/'04-dark-warm-decor-sheet.png')
    (out/'decor-review.json').write_text(json.dumps(dict(sources=sources,palette=palette,approximations=APPROXIMATIONS,
                                                       missing=sorted(renderer.missing)), indent=2)+'\n')
