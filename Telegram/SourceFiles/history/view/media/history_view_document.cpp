/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/media/history_view_document.h"

#include "lang/lang_keys.h"
#include "storage/localstorage.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_instance.h"
#include "history/history_item_components.h"
#include "history/history.h"
#include "history/view/history_view_element.h"
#include "history/view/history_view_cursor_state.h"
#include "history/view/history_view_transcribe_button.h"
#include "history/view/media/history_view_media_common.h"
#include "ui/image/image.h"
#include "ui/text/format_values.h"
#include "ui/text/format_song_document_name.h"
#include "ui/text/text_utilities.h"
#include "ui/chat/message_bubble.h"
#include "ui/chat/chat_style.h"
#include "ui/cached_round_corners.h"
#include "ui/ui_utility.h"
#include "data/data_session.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_document_resolver.h"
#include "data/data_media_types.h"
#include "data/data_file_click_handler.h"
#include "data/data_file_origin.h"
#include "api/api_transcribes.h"
#include "apiwrap.h"
#include "styles/style_chat.h"

namespace HistoryView {
namespace {

constexpr auto kAudioVoiceMsgUpdateView = crl::time(100);

[[nodiscard]] QString CleanTagSymbols(const QString &value) {
	auto result = QString();
	const auto begin = value.begin(), end = value.end();
	auto from = begin;
	for (auto ch = begin; ch != end; ++ch) {
		if (ch->isHighSurrogate()
			&& (ch + 1) != end
			&& (ch + 1)->isLowSurrogate()
			&& QChar::surrogateToUcs4(
				ch->unicode(),
				(ch + 1)->unicode()) >= 0xe0000) {
			if (ch > from) {
				if (result.isEmpty()) {
					result.reserve(value.size());
				}
				result.append(from, ch - from);
			}
			++ch;
			from = ch + 1;
		}
	}
	if (from == begin) {
		return value;
	} else if (end > from) {
		result.append(from, end - from);
	}
	return result;
}

void PaintWaveform(
		Painter &p,
		const PaintContext &context,
		const VoiceData *voiceData,
		int availableWidth,
		float64 progress) {
	const auto wf = [&]() -> const VoiceWaveform* {
		if (!voiceData) {
			return nullptr;
		}
		if (voiceData->waveform.isEmpty()) {
			return nullptr;
		} else if (voiceData->waveform.at(0) < 0) {
			return nullptr;
		}
		return &voiceData->waveform;
	}();
	const auto stm = context.messageStyle();

	// Rescale waveform by going in waveform.size * bar_count 1D grid.
	const auto active = stm->msgWaveformActive;
	const auto inactive = stm->msgWaveformInactive;
	const auto wfSize = wf
		? int(wf->size())
		: ::Media::Player::kWaveformSamplesCount;
	const auto activeWidth = base::SafeRound(availableWidth * progress);

	const auto &barWidth = st::msgWaveformBar;
	const auto barCount = std::min(
		availableWidth / (barWidth + st::msgWaveformSkip),
		wfSize);
	const auto barNormValue = (wf ? voiceData->wavemax : 0) + 1;
	const auto maxDelta = st::msgWaveformMax - st::msgWaveformMin;
	p.setPen(Qt::NoPen);
	auto hq = PainterHighQualityEnabler(p);
	for (auto i = 0, barLeft = 0, sum = 0, maxValue = 0; i < wfSize; ++i) {
		const auto value = wf ? wf->at(i) : 0;
		if (sum + barCount < wfSize) {
			maxValue = std::max(maxValue, value);
			sum += barCount;
			continue;
		}
		// Draw bar.
		sum = sum + barCount - wfSize;
		if (sum < (barCount + 1) / 2) {
			maxValue = std::max(maxValue, value);
		}
		const auto barValue = ((maxValue * maxDelta) + (barNormValue / 2))
			/ barNormValue;
		const auto barHeight = st::msgWaveformMin + barValue;
		const auto barTop = st::lineWidth + (st::msgWaveformMax - barValue) / 2.;

		if ((barLeft < activeWidth) && (barLeft + barWidth > activeWidth)) {
			const auto leftWidth = activeWidth - barLeft;
			const auto rightWidth = barWidth - leftWidth;
			p.fillRect(
				QRectF(barLeft, barTop, leftWidth, barHeight),
				active);
			p.fillRect(
				QRectF(activeWidth, barTop, rightWidth, barHeight),
				inactive);
		} else {
			const auto &color = (barLeft >= activeWidth) ? inactive : active;
			p.fillRect(QRectF(barLeft, barTop, barWidth, barHeight), color);
		}
		barLeft += barWidth + st::msgWaveformSkip;

		maxValue = (sum < (barCount + 1) / 2) ? 0 : value;
	}
}

[[nodiscard]] int MaxStatusWidth(not_null<DocumentData*> document) {
	using namespace Ui;
	auto result = 0;
	const auto add = [&](const QString &text) {
		accumulate_max(result, st::normalFont->width(text));
	};
	add(FormatDownloadText(document->size, document->size));
	const auto duration = document->getDuration();
	if (const auto song = document->song()) {
		add(FormatPlayedText(duration, duration));
		add(FormatDurationAndSizeText(duration, document->size));
	} else if (const auto voice = document->voice()) {
		add(FormatPlayedText(duration, duration));
		add(FormatDurationAndSizeText(duration, document->size));
	} else if (document->isVideoFile()) {
		add(FormatDurationAndSizeText(duration, document->size));
	} else {
		add(FormatSizeText(document->size));
	}
	return result;
}

} // namespace

Document::Document(
	not_null<Element*> parent,
	not_null<HistoryItem*> realParent,
	not_null<DocumentData*> document)
: File(parent, realParent)
, _data(document) {
	auto caption = createCaption();

	createComponents(!caption.isEmpty());
	if (const auto named = Get<HistoryDocumentNamed>()) {
		fillNamedFromData(named);
	}

	setDocumentLinks(_data, realParent);

	setStatusSize(Ui::FileStatusSizeReady);

	if (const auto captioned = Get<HistoryDocumentCaptioned>()) {
		captioned->_caption = std::move(caption);
	}
}

Document::~Document() {
	if (_dataMedia) {
		_data->owner().keepAlive(base::take(_dataMedia));
		_parent->checkHeavyPart();
	}
}

float64 Document::dataProgress() const {
	ensureDataMediaCreated();
	return _dataMedia->progress();
}

bool Document::dataFinished() const {
	return !_data->loading()
		&& (!_data->uploading() || _data->waitingForAlbum());
}

bool Document::dataLoaded() const {
	ensureDataMediaCreated();
	return _dataMedia->loaded();
}

void Document::createComponents(bool caption) {
	uint64 mask = 0;
	if (_data->isVoiceMessage()) {
		mask |= HistoryDocumentVoice::Bit();
	} else {
		mask |= HistoryDocumentNamed::Bit();
		if (_data->hasThumbnail()) {
			if (!_data->isSong()
				&& !Data::IsExecutableName(_data->filename())) {
				_data->loadThumbnail(_realParent->fullId());
				mask |= HistoryDocumentThumbed::Bit();
			}
		}
	}
	if (caption) {
		mask |= HistoryDocumentCaptioned::Bit();
	}
	UpdateComponents(mask);
	if (const auto thumbed = Get<HistoryDocumentThumbed>()) {
		thumbed->_linksavel = std::make_shared<DocumentSaveClickHandler>(
			_data,
			_realParent->fullId());
		thumbed->_linkopenwithl = std::make_shared<DocumentOpenWithClickHandler>(
			_data,
			_realParent->fullId());
		thumbed->_linkcancell = std::make_shared<DocumentCancelClickHandler>(
			_data,
			crl::guard(this, [=](FullMsgId id) {
				_parent->delegate()->elementCancelUpload(id);
			}),
			_realParent->fullId());
	}
	if (const auto voice = Get<HistoryDocumentVoice>()) {
		voice->_seekl = std::make_shared<VoiceSeekClickHandler>(
			_data,
			[](FullMsgId) {});
	}
}

void Document::fillNamedFromData(HistoryDocumentNamed *named) {
	const auto nameString = named->_name = CleanTagSymbols(
		Ui::Text::FormatSongNameFor(_data).string());
	named->_namew = st::semiboldFont->width(nameString);
}

QSize Document::countOptimalSize() {
	auto captioned = Get<HistoryDocumentCaptioned>();
	if (_parent->media() != this && !_realParent->groupId()) {
		if (captioned) {
			RemoveComponents(HistoryDocumentCaptioned::Bit());
			captioned = nullptr;
		}
	} else if (captioned && captioned->_caption.hasSkipBlock()) {
		captioned->_caption.updateSkipBlock(
			_parent->skipBlockWidth(),
			_parent->skipBlockHeight());
	}

	auto hasTranscribe = false;
	const auto voice = Get<HistoryDocumentVoice>();
	if (voice) {
		const auto session = &_realParent->history()->session();
		if (!session->premium()) {
			voice->transcribe = nullptr;
			voice->transcribeText = {};
		} else {
			const auto creating = !voice->transcribe;
			if (creating) {
				voice->transcribe = std::make_unique<TranscribeButton>(
					_realParent);
			}
			const auto &entry = session->api().transcribes().entry(
				_realParent);
			const auto update = [=] { repaint(); };
			voice->transcribe->setLoading(
				entry.shown && (entry.requestId || entry.pending),
				update);
			auto text = (entry.requestId || !entry.shown)
				? TextWithEntities()
				: entry.toolong
				? Ui::Text::Italic(tr::lng_audio_transcribe_long(tr::now))
				: entry.failed
				? Ui::Text::Italic(tr::lng_attach_failed(tr::now))
				: TextWithEntities{
					entry.result + (entry.pending ? " [...]" : ""),
				};
			voice->transcribe->setOpened(
				!text.empty(),
				creating ? Fn<void()>() : update);
			if (text.empty()) {
				voice->transcribeText = {};
			} else {
				const auto minResizeWidth = st::minPhotoSize
					- st::msgPadding.left()
					- st::msgPadding.right();
				voice->transcribeText = Ui::Text::String(minResizeWidth);
				voice->transcribeText.setMarkedText(
					st::messageTextStyle,
					text);
				hasTranscribe = true;
				if (const auto skipBlockWidth = captioned
					? 0
					: _parent->skipBlockWidth()) {
					voice->transcribeText.updateSkipBlock(
						skipBlockWidth,
						_parent->skipBlockHeight());
				}
			}
		}
	}

	auto thumbed = Get<HistoryDocumentThumbed>();
	const auto &st = thumbed ? st::msgFileThumbLayout : st::msgFileLayout;
	if (thumbed) {
		const auto &location = _data->thumbnailLocation();
		auto tw = style::ConvertScale(location.width());
		auto th = style::ConvertScale(location.height());
		if (tw > th) {
			thumbed->_thumbw = (tw * st.thumbSize) / th;
		} else {
			thumbed->_thumbw = st.thumbSize;
		}
	}

	auto maxWidth = st::msgFileMinWidth;

	const auto tleft = st.padding.left() + st.thumbSize + st.padding.right();
	const auto tright = st.padding.left();
	if (thumbed) {
		accumulate_max(maxWidth, tleft + MaxStatusWidth(_data) + tright);
	} else {
		auto unread = _data->isVoiceMessage() ? (st::mediaUnreadSkip + st::mediaUnreadSize) : 0;
		accumulate_max(maxWidth, tleft + MaxStatusWidth(_data) + unread + _parent->skipBlockWidth() + st::msgPadding.right());
	}

	if (auto named = Get<HistoryDocumentNamed>()) {
		accumulate_max(maxWidth, tleft + named->_namew + tright);
		accumulate_min(maxWidth, st::msgMaxWidth);
	}
	if (voice && voice->transcribe) {
		maxWidth += st::historyTranscribeSkip
			+ voice->transcribe->size().width();
	}

	auto minHeight = st.padding.top() + st.thumbSize + st.padding.bottom();
	if (!captioned && !hasTranscribe && _parent->bottomInfoIsWide()) {
		minHeight += st::msgDateFont->height - st::msgDateDelta.y();
	}
	if (!isBubbleTop()) {
		minHeight -= st::msgFileTopMinus;
	}

	if (hasTranscribe) {
		auto captionw = maxWidth
			- st::msgPadding.left()
			- st::msgPadding.right();
		minHeight += voice->transcribeText.countHeight(captionw);
		if (captioned) {
			minHeight += st::mediaCaptionSkip;
		} else if (isBubbleBottom()) {
			minHeight += st::msgPadding.bottom();
		}
	}
	if (captioned) {
		auto captionw = maxWidth
			- st::msgPadding.left()
			- st::msgPadding.right();
		minHeight += captioned->_caption.countHeight(captionw);
		if (isBubbleBottom()) {
			minHeight += st::msgPadding.bottom();
		}
	}
	return { maxWidth, minHeight };
}

QSize Document::countCurrentSize(int newWidth) {
	const auto captioned = Get<HistoryDocumentCaptioned>();
	const auto voice = Get<HistoryDocumentVoice>();
	const auto hasTranscribe = voice && !voice->transcribeText.isEmpty();
	if (!captioned && !hasTranscribe) {
		return File::countCurrentSize(newWidth);
	}

	accumulate_min(newWidth, maxWidth());
	const auto thumbed = Get<HistoryDocumentThumbed>();
	const auto &st = thumbed ? st::msgFileThumbLayout : st::msgFileLayout;
	auto newHeight = st.padding.top() + st.thumbSize + st.padding.bottom();
	if (!isBubbleTop()) {
		newHeight -= st::msgFileTopMinus;
	}
	auto captionw = newWidth - st::msgPadding.left() - st::msgPadding.right();
	if (hasTranscribe) {
		newHeight += voice->transcribeText.countHeight(captionw);
		if (captioned) {
			newHeight += st::mediaCaptionSkip;
		} else if (isBubbleBottom()) {
			newHeight += st::msgPadding.bottom();
		}
	}
	if (captioned) {
		newHeight += captioned->_caption.countHeight(captionw);
		if (isBubbleBottom()) {
			newHeight += st::msgPadding.bottom();
		}
	}

	return { newWidth, newHeight };
}

void Document::draw(Painter &p, const PaintContext &context) const {
	draw(p, context, width(), LayoutMode::Full);
}

void Document::draw(
		Painter &p,
		const PaintContext &context,
		int width,
		LayoutMode mode) const {
	if (width < st::msgPadding.left() + st::msgPadding.right() + 1) return;

	ensureDataMediaCreated();

	const auto cornerDownload = downloadInCorner();

	if (!_dataMedia->canBePlayed(_realParent)) {
		_dataMedia->automaticLoad(_realParent->fullId(), _realParent);
	}
	bool loaded = dataLoaded(), displayLoading = _data->displayLoading();
	const auto sti = context.imageStyle();
	const auto stm = context.messageStyle();

	int captionw = width - st::msgPadding.left() - st::msgPadding.right();

	if (displayLoading) {
		ensureAnimation();
		if (!_animation->radial.animating()) {
			_animation->radial.start(dataProgress());
		}
	}
	const auto showPause = updateStatusText();
	const auto radial = isRadialAnimation();

	const auto topMinus = isBubbleTop() ? 0 : st::msgFileTopMinus;
	const auto thumbed = Get<HistoryDocumentThumbed>();
	const auto &st = (mode == LayoutMode::Full)
		? (thumbed ? st::msgFileThumbLayout : st::msgFileLayout)
		: (thumbed ? st::msgFileThumbLayoutGrouped : st::msgFileLayoutGrouped);
	const auto nameleft = st.padding.left() + st.thumbSize + st.padding.right();
	const auto nametop = st.nameTop - topMinus;
	const auto nameright = st.padding.left();
	const auto statustop = st.statusTop - topMinus;
	const auto linktop = st.linkTop - topMinus;
	const auto bottom = st.padding.top() + st.thumbSize + st.padding.bottom() - topMinus;
	const auto rthumb = style::rtlrect(st.padding.left(), st.padding.top() - topMinus, st.thumbSize, st.thumbSize, width);
	const auto innerSize = st::msgFileLayout.thumbSize;
	const auto inner = QRect(rthumb.x() + (rthumb.width() - innerSize) / 2, rthumb.y() + (rthumb.height() - innerSize) / 2, innerSize, innerSize);
	const auto radialOpacity = radial ? _animation->radial.opacity() : 1.;
	if (thumbed) {
		const auto inWebPage = (_parent->media() != this);
		const auto args = Images::PrepareArgs{
			.options = (inWebPage
				? Images::Option::RoundSmall
				: Images::Option::RoundLarge),
			.outer = QSize(st.thumbSize, st.thumbSize),
		};
		QPixmap thumb;
		if (const auto normal = _dataMedia->thumbnail()) {
			thumb = normal->pixSingle(thumbed->_thumbw, args);
		} else if (const auto blurred = _dataMedia->thumbnailInline()) {
			thumb = blurred->pixSingle(thumbed->_thumbw, args.blurred());
		}
		p.drawPixmap(rthumb.topLeft(), thumb);
		if (context.selected()) {
			const auto st = context.st;
			Ui::FillRoundRect(p, rthumb, st->msgSelectOverlay(), inWebPage
				? st->msgSelectOverlayCornersSmall()
				: st->msgSelectOverlayCornersLarge());
		}

		if (radial || (!loaded && !_data->loading()) || _data->waitingForAlbum()) {
			const auto backOpacity = (loaded && !_data->uploading()) ? radialOpacity : 1.;
			p.setPen(Qt::NoPen);
			p.setBrush(sti->msgDateImgBg);
			p.setOpacity(backOpacity * p.opacity());

			{
				PainterHighQualityEnabler hq(p);
				p.drawEllipse(inner);
			}

			const auto &icon = _data->waitingForAlbum()
				? sti->historyFileThumbWaiting
				: (radial || _data->loading())
				? sti->historyFileThumbCancel
				: sti->historyFileThumbDownload;
			const auto previous = _data->waitingForAlbum()
				? &sti->historyFileThumbCancel
				: nullptr;
			p.setOpacity(backOpacity);
			if (previous && radialOpacity > 0. && radialOpacity < 1.) {
				PaintInterpolatedIcon(p, icon, *previous, radialOpacity, inner);
			} else {
				icon.paintInCenter(p, inner);
			}
			p.setOpacity(1.);
			if (radial) {
				QRect rinner(inner.marginsRemoved(QMargins(st::msgFileRadialLine, st::msgFileRadialLine, st::msgFileRadialLine, st::msgFileRadialLine)));
				_animation->radial.draw(p, rinner, st::msgFileRadialLine, sti->historyFileThumbRadialFg);
			}
		}

		if (_data->status != FileUploadFailed) {
			const auto &lnk = (_data->loading() || _data->uploading())
				? thumbed->_linkcancell
				: dataLoaded()
				? thumbed->_linkopenwithl
				: thumbed->_linksavel;
			bool over = ClickHandler::showAsActive(lnk);
			p.setFont(over ? st::semiboldFont->underline() : st::semiboldFont);
			p.setPen(stm->msgFileThumbLinkFg);
			p.drawTextLeft(nameleft, linktop, width, thumbed->_link, thumbed->_linkw);
		}
	} else {
		p.setPen(Qt::NoPen);

		const auto coverDrawn = _data->isSongWithCover()
			&& DrawThumbnailAsSongCover(
				p,
				context.st->songCoverOverlayFg(),
				_dataMedia,
				inner,
				context.selected());
		if (!coverDrawn) {
			PainterHighQualityEnabler hq(p);
			p.setBrush(stm->msgFileBg);
			p.drawEllipse(inner);
		}

		const auto &icon = [&]() -> const style::icon& {
			if (_data->waitingForAlbum()) {
				return _data->isSongWithCover()
					? sti->historyFileThumbWaiting
					: stm->historyFileWaiting;
			} else if (!cornerDownload
				&& (_data->loading() || _data->uploading())) {
				return _data->isSongWithCover()
					? sti->historyFileThumbCancel
					: stm->historyFileCancel;
			} else if (showPause) {
				return _data->isSongWithCover()
					? sti->historyFileThumbPause
					: stm->historyFilePause;
			} else if (loaded || _dataMedia->canBePlayed(_realParent)) {
				return _dataMedia->canBePlayed(_realParent)
					? (_data->isSongWithCover()
						? sti->historyFileThumbPlay
						: stm->historyFilePlay)
					: _data->isImage()
					? stm->historyFileImage
					: stm->historyFileDocument;
			} else {
				return _data->isSongWithCover()
					? sti->historyFileThumbDownload
					: stm->historyFileDownload;
			}
		}();
		const auto previous = _data->waitingForAlbum()
			? &stm->historyFileCancel
			: nullptr;

		const auto paintContent = [&](Painter &q) {
			if (previous && radialOpacity > 0. && radialOpacity < 1.) {
				PaintInterpolatedIcon(q, icon, *previous, radialOpacity, inner);
			} else {
				icon.paintInCenter(q, inner);
			}

			if (radial && !cornerDownload) {
				QRect rinner(inner.marginsRemoved(QMargins(st::msgFileRadialLine, st::msgFileRadialLine, st::msgFileRadialLine, st::msgFileRadialLine)));
				_animation->radial.draw(q, rinner, st::msgFileRadialLine, stm->historyFileRadialFg);
			}
		};
		if (_data->isSongWithCover() || !usesBubblePattern(context)) {
			paintContent(p);
		} else {
			Ui::PaintPatternBubblePart(
				p,
				context.viewport,
				context.bubblesPattern->pixmap,
				inner,
				paintContent,
				_iconCache);
		}

		drawCornerDownload(p, context, mode);
	}
	auto namewidth = width - nameleft - nameright;
	auto statuswidth = namewidth;

	auto voiceStatusOverride = QString();
	const auto voice = Get<HistoryDocumentVoice>();
	if (voice) {
		ensureDataMediaCreated();

		if (const auto voiceData = _data->voice()) {
			if (voiceData->waveform.isEmpty()) {
				if (loaded) {
					Local::countVoiceWaveform(_dataMedia.get());
				}
			}
		}

		const auto progress = [&] {
			if (!context.outbg
				&& !voice->_playback
				&& _realParent->hasUnreadMediaFlag()) {
				return 1.;
			}
			if (voice->seeking()) {
				return voice->seekingCurrent();
			} else if (voice->_playback) {
				return voice->_playback->progress.current();
			}
			return 0.;
		}();
		if (voice->seeking()) {
			voiceStatusOverride = Ui::FormatPlayedText(
				base::SafeRound(progress * voice->_lastDurationMs) / 1000,
				voice->_lastDurationMs / 1000);
		}
		if (voice->transcribe) {
			const auto size = voice->transcribe->size();
			namewidth -= st::historyTranscribeSkip + size.width();
			const auto x = nameleft + namewidth + st::historyTranscribeSkip;
			const auto y = st.padding.top() - topMinus;
			voice->transcribe->paint(p, x, y, context);
		}
		p.save();
		p.translate(nameleft, st.padding.top() - topMinus);

		PaintWaveform(p,
			context,
			_data->voice(),
			namewidth + st::msgWaveformSkip,
			progress);
		p.restore();
	} else if (auto named = Get<HistoryDocumentNamed>()) {
		p.setFont(st::semiboldFont);
		p.setPen(stm->historyFileNameFg);
		if (namewidth < named->_namew) {
			p.drawTextLeft(nameleft, nametop, width, st::semiboldFont->elided(named->_name, namewidth, Qt::ElideMiddle));
		} else {
			p.drawTextLeft(nameleft, nametop, width, named->_name, named->_namew);
		}
	}

	auto statusText = voiceStatusOverride.isEmpty() ? _statusText : voiceStatusOverride;
	p.setFont(st::normalFont);
	p.setPen(stm->mediaFg);
	p.drawTextLeft(nameleft, statustop, width, statusText);

	if (_realParent->hasUnreadMediaFlag()) {
		auto w = st::normalFont->width(statusText);
		if (w + st::mediaUnreadSkip + st::mediaUnreadSize <= statuswidth) {
			p.setPen(Qt::NoPen);
			p.setBrush(stm->msgFileBg);

			{
				PainterHighQualityEnabler hq(p);
				p.drawEllipse(style::rtlrect(nameleft + w + st::mediaUnreadSkip, statustop + st::mediaUnreadTop, st::mediaUnreadSize, st::mediaUnreadSize, width));
			}
		}
	}

	auto selection = context.selection;
	auto captiontop = bottom;
	if (voice && !voice->transcribeText.isEmpty()) {
		p.setPen(stm->historyTextFg);
		voice->transcribeText.draw(p, st::msgPadding.left(), bottom, captionw, style::al_left, 0, -1, selection);
		captiontop += voice->transcribeText.countHeight(captionw) + st::mediaCaptionSkip;
		selection = HistoryView::UnshiftItemSelection(selection, voice->transcribeText);
	}
	if (const auto captioned = Get<HistoryDocumentCaptioned>()) {
		p.setPen(stm->historyTextFg);
		_parent->prepareCustomEmojiPaint(p, context, captioned->_caption);
		captioned->_caption.draw(p, st::msgPadding.left(), captiontop, captionw, style::al_left, 0, -1, selection);
	}
}

bool Document::hasHeavyPart() const {
	return (_dataMedia != nullptr);
}

void Document::unloadHeavyPart() {
	_dataMedia = nullptr;
	if (const auto captioned = Get<HistoryDocumentCaptioned>()) {
		captioned->_caption.unloadCustomEmoji();
	}
}

void Document::ensureDataMediaCreated() const {
	if (_dataMedia) {
		return;
	}
	_dataMedia = _data->createMediaView();
	if (Get<HistoryDocumentThumbed>() || _data->isSongWithCover()) {
		_dataMedia->thumbnailWanted(_realParent->fullId());
	}
	history()->owner().registerHeavyViewPart(_parent);
}

bool Document::downloadInCorner() const {
	return _data->isAudioFile()
		&& _realParent->allowsForward()
		&& _data->canBeStreamed(_realParent)
		&& !_data->inappPlaybackFailed();
}

void Document::drawCornerDownload(
		Painter &p,
		const PaintContext &context,
		LayoutMode mode) const {
	if (dataLoaded()
		|| _data->loadedInMediaCache()
		|| !downloadInCorner()) {
		return;
	}
	auto topMinus = isBubbleTop() ? 0 : st::msgFileTopMinus;
	const auto stm = context.messageStyle();
	const auto thumbed = false;
	const auto &st = (mode == LayoutMode::Full)
		? (thumbed ? st::msgFileThumbLayout : st::msgFileLayout)
		: (thumbed ? st::msgFileThumbLayoutGrouped : st::msgFileLayoutGrouped);
	const auto shift = st::historyAudioDownloadShift;
	const auto size = st::historyAudioDownloadSize;
	const auto inner = style::rtlrect(st.padding.left() + shift, st.padding.top() - topMinus + shift, size, size, width());
	const auto bubblePattern = usesBubblePattern(context);
	if (bubblePattern) {
		p.setPen(Qt::NoPen);
	} else {
		auto pen = stm->msgBg->p;
		pen.setWidth(st::lineWidth);
		p.setPen(pen);
	}
	p.setBrush(stm->msgFileBg);
	{
		PainterHighQualityEnabler hq(p);
		p.drawEllipse(inner);
	}
	const auto &icon = _data->loading()
		? stm->historyAudioCancel
		: stm->historyAudioDownload;
	const auto paintContent = [&](Painter &q) {
		if (bubblePattern) {
			auto hq = PainterHighQualityEnabler(q);
			auto pen = stm->msgBg->p;
			pen.setWidth(st::lineWidth);
			q.setPen(pen);
			q.setBrush(Qt::NoBrush);
			q.drawEllipse(inner);
		}
		icon.paintInCenter(q, inner);
		if (_animation && _animation->radial.animating()) {
			const auto rinner = inner.marginsRemoved(QMargins(st::historyAudioRadialLine, st::historyAudioRadialLine, st::historyAudioRadialLine, st::historyAudioRadialLine));
			_animation->radial.draw(q, rinner, st::historyAudioRadialLine, stm->historyFileRadialFg);
		}
	};
	if (bubblePattern) {
		const auto add = st::lineWidth * 2;
		const auto target = inner.marginsAdded({ add, add, add, add });
		Ui::PaintPatternBubblePart(
			p,
			context.viewport,
			context.bubblesPattern->pixmap,
			target,
			paintContent,
			_cornerDownloadCache);
	} else {
		paintContent(p);
	}
}

TextState Document::cornerDownloadTextState(
		QPoint point,
		StateRequest request,
		LayoutMode mode) const {
	auto result = TextState(_parent);
	if (dataLoaded()
		|| _data->loadedInMediaCache()
		|| !downloadInCorner()) {
		return result;
	}
	auto topMinus = isBubbleTop() ? 0 : st::msgFileTopMinus;
	const auto thumbed = false;
	const auto &st = (mode == LayoutMode::Full)
		? (thumbed ? st::msgFileThumbLayout : st::msgFileLayout)
		: (thumbed ? st::msgFileThumbLayoutGrouped : st::msgFileLayoutGrouped);
	const auto shift = st::historyAudioDownloadShift;
	const auto size = st::historyAudioDownloadSize;
	const auto inner = style::rtlrect(st.padding.left() + shift, st.padding.top() - topMinus + shift, size, size, width());
	if (inner.contains(point)) {
		result.link = _data->loading() ? _cancell : _savel;
	}
	return result;
}

TextState Document::textState(QPoint point, StateRequest request) const {
	return textState(point, { width(), height() }, request, LayoutMode::Full);
}

TextState Document::textState(
		QPoint point,
		QSize layout,
		StateRequest request,
		LayoutMode mode) const {
	const auto width = layout.width();

	auto result = TextState(_parent);

	if (width < st::msgPadding.left() + st::msgPadding.right() + 1) {
		return result;
	}

	ensureDataMediaCreated();
	bool loaded = dataLoaded();

	updateStatusText();

	const auto topMinus = isBubbleTop() ? 0 : st::msgFileTopMinus;
	const auto thumbed = Get<HistoryDocumentThumbed>();
	const auto &st = (mode == LayoutMode::Full)
		? (thumbed ? st::msgFileThumbLayout : st::msgFileLayout)
		: (thumbed ? st::msgFileThumbLayoutGrouped : st::msgFileLayoutGrouped);
	const auto nameleft = st.padding.left() + st.thumbSize + st.padding.right();
	const auto nametop = st.nameTop - topMinus;
	const auto nameright = st.padding.left();
	const auto linktop = st.linkTop - topMinus;
	auto bottom = st.padding.top() + st.thumbSize + st.padding.bottom() - topMinus;
	const auto rthumb = style::rtlrect(st.padding.left(), st.padding.top() - topMinus, st.thumbSize, st.thumbSize, width);
	const auto innerSize = st::msgFileLayout.thumbSize;
	const auto inner = QRect(rthumb.x() + (rthumb.width() - innerSize) / 2, rthumb.y() + (rthumb.height() - innerSize) / 2, innerSize, innerSize);
	if (const auto thumbed = Get<HistoryDocumentThumbed>()) {
		if ((_data->loading() || _data->uploading()) && rthumb.contains(point)) {
			result.link = _cancell;
			return result;
		}

		if (_data->status != FileUploadFailed) {
			if (style::rtlrect(nameleft, linktop, thumbed->_linkw, st::semiboldFont->height, width).contains(point)) {
				result.link = (_data->loading() || _data->uploading())
					? thumbed->_linkcancell
					: dataLoaded()
					? thumbed->_linkopenwithl
					: thumbed->_linksavel;
				return result;
			}
		}
	} else {
		if (const auto state = cornerDownloadTextState(point, request, mode); state.link) {
			return state;
		}
		if ((_data->loading() || _data->uploading()) && inner.contains(point) && !downloadInCorner()) {
			result.link = _cancell;
			return result;
		}
	}

	const auto voice = Get<HistoryDocumentVoice>();
	auto namewidth = width - nameleft - nameright;
	auto transcribeLength = 0;
	auto transcribeHeight = 0;
	auto painth = layout.height();
	if (voice) {
		auto waveformbottom = st.padding.top() - topMinus + st::msgWaveformMax + st::msgWaveformMin;
		if (voice->transcribe) {
			const auto size = voice->transcribe->size();
			namewidth -= st::historyTranscribeSkip + size.width();
			const auto x = nameleft + namewidth + st::historyTranscribeSkip;
			const auto y = st.padding.top() - topMinus;
			if (QRect(QPoint(x, y), size).contains(point)) {
				result.link = voice->transcribe->link();
				return result;
			}
		}
		if (QRect(nameleft, nametop, namewidth, waveformbottom - nametop).contains(point)) {
			const auto state = ::Media::Player::instance()->getState(AudioMsgId::Type::Voice);
			if (state.id == AudioMsgId(_data, _realParent->fullId(), state.id.externalPlayId())
				&& !::Media::Player::IsStoppedOrStopping(state.state)) {
				if (!voice->seeking()) {
					voice->setSeekingStart((point.x() - nameleft) / float64(namewidth));
				}
				result.link = voice->_seekl;
				return result;
			}
		}
		transcribeLength = voice->transcribeText.length();
		if (transcribeLength > 0) {
			auto captionw = width - st::msgPadding.left() - st::msgPadding.right();
			transcribeHeight = voice->transcribeText.countHeight(captionw);
			painth -= transcribeHeight;
			if (point.y() >= bottom && point.y() < bottom + transcribeHeight) {
				result = TextState(_parent, voice->transcribeText.getState(
					point - QPoint(st::msgPadding.left(), bottom),
					width - st::msgPadding.left() - st::msgPadding.right(),
					request.forText()));
				return result;
			}
			bottom += transcribeHeight;
		}
	}

	if (const auto captioned = Get<HistoryDocumentCaptioned>()) {
		if (point.y() >= bottom) {
			result.symbol += transcribeLength;
		}
		if (transcribeHeight) {
			painth -= st::mediaCaptionSkip;
			bottom += st::mediaCaptionSkip;
		}
		if (point.y() >= bottom) {
			result = TextState(_parent, captioned->_caption.getState(
				point - QPoint(st::msgPadding.left(), bottom),
				width - st::msgPadding.left() - st::msgPadding.right(),
				request.forText()));
			result.symbol += transcribeLength;
			return result;
		}
		auto captionw = width - st::msgPadding.left() - st::msgPadding.right();
		painth -= captioned->_caption.countHeight(captionw);
		if (isBubbleBottom()) {
			painth -= st::msgPadding.bottom();
		}
	} else if (transcribeHeight && isBubbleBottom()) {
		painth -= st::msgPadding.bottom();
	}
	const auto till = voice ? (nameleft + namewidth) : width;
	if (QRect(0, 0, till, painth).contains(point)
		&& (!_data->loading() || downloadInCorner())
		&& !_data->uploading()
		&& !_data->isNull()) {
		if (loaded || _dataMedia->canBePlayed(_realParent)) {
			result.link = _openl;
		} else {
			result.link = _savel;
		}
		return result;
	}
	return result;
}

void Document::updatePressed(QPoint point) {
	// LayoutMode should be passed here.
	if (const auto voice = Get<HistoryDocumentVoice>()) {
		if (voice->seeking()) {
			const auto thumbed = Get<HistoryDocumentThumbed>();
			const auto &st = thumbed ? st::msgFileThumbLayout : st::msgFileLayout;
			const auto nameleft = st.padding.left() + st.thumbSize + st.padding.right();
			const auto nameright = st.padding.left();
			voice->setSeekingCurrent(std::clamp(
				(point.x() - nameleft)
					/ float64(width() - nameleft - nameright),
				0.,
				1.));
			repaint();
		}
	}
}

TextSelection Document::adjustSelection(
		TextSelection selection,
		TextSelectType type) const {
	auto transcribe = (const Ui::Text::String*)nullptr;
	auto caption = (const Ui::Text::String*)nullptr;
	if (const auto voice = Get<HistoryDocumentVoice>()) {
		transcribe = &voice->transcribeText;
	}
	if (const auto captioned = Get<HistoryDocumentCaptioned>()) {
		caption = &captioned->_caption;
	}
	const auto transcribeLength = transcribe ? transcribe->length() : 0;
	if (transcribe && selection.from < transcribeLength) {
		const auto adjusted = transcribe->adjustSelection(selection, type);
		if (selection.to <= transcribeLength) {
			return adjusted;
		}
		selection = TextSelection(adjusted.from, selection.to);
	}
	if (caption && selection.to > transcribeLength) {
		auto unshifted = transcribe
			? HistoryView::UnshiftItemSelection(selection, *transcribe)
			: selection;
		const auto adjusted = caption->adjustSelection(unshifted, type);
		const auto shifted = transcribe
			? HistoryView::ShiftItemSelection(adjusted, *transcribe)
			: adjusted;
		if (selection.from >= transcribeLength) {
			return shifted;
		}
		selection = TextSelection(selection.from, shifted.to);
	}
	return selection;
}

uint16 Document::fullSelectionLength() const {
	auto result = uint16();
	if (const auto voice = Get<HistoryDocumentVoice>()) {
		result += voice->transcribeText.length();
	}
	if (const auto captioned = Get<HistoryDocumentCaptioned>()) {
		result += captioned->_caption.length();
	}
	return result;
}

bool Document::hasTextForCopy() const {
	if (const auto voice = Get<HistoryDocumentVoice>()) {
		if (!voice->transcribeText.isEmpty()) {
			return true;
		}
	}
	return Has<HistoryDocumentCaptioned>();
}

TextForMimeData Document::selectedText(TextSelection selection) const {
	auto result = TextForMimeData();
	if (const auto voice = Get<HistoryDocumentVoice>()) {
		const auto length = voice->transcribeText.length();
		if (selection.from < length) {
			result.append(
				voice->transcribeText.toTextForMimeData(selection));
		}
		if (selection.to <= length) {
			return result;
		}
		selection = HistoryView::UnshiftItemSelection(
			selection,
			voice->transcribeText);
	}
	if (const auto captioned = Get<HistoryDocumentCaptioned>()) {
		if (!result.empty()) {
			result.append("\n\n");
		}
		result.append(captioned->_caption.toTextForMimeData(selection));
	}
	return result;
}

bool Document::uploading() const {
	return _data->uploading();
}

void Document::setStatusSize(int64 newSize, TimeId realDuration) const {
	TimeId duration = _data->isSong()
		? _data->song()->duration
		: (_data->isVoiceMessage()
			? _data->voice()->duration
			: -1);
	File::setStatusSize(newSize, _data->size, duration, realDuration);
	if (auto thumbed = Get<HistoryDocumentThumbed>()) {
		if (_statusSize == Ui::FileStatusSizeReady) {
			thumbed->_link = tr::lng_media_download(tr::now).toUpper();
		} else if (_statusSize == Ui::FileStatusSizeLoaded) {
			thumbed->_link = tr::lng_media_open_with(tr::now).toUpper();
		} else if (_statusSize == Ui::FileStatusSizeFailed) {
			thumbed->_link = tr::lng_media_download(tr::now).toUpper();
		} else if (_statusSize >= 0) {
			thumbed->_link = tr::lng_media_cancel(tr::now).toUpper();
		} else {
			thumbed->_link = tr::lng_media_open_with(tr::now).toUpper();
		}
		thumbed->_linkw = st::semiboldFont->width(thumbed->_link);
	}
}

bool Document::updateStatusText() const {
	auto showPause = false;
	auto statusSize = int64();
	auto realDuration = TimeId();
	if (_data->status == FileDownloadFailed || _data->status == FileUploadFailed) {
		statusSize = Ui::FileStatusSizeFailed;
	} else if (_data->uploading()) {
		statusSize = _data->uploadingData->offset;
	} else if (_data->loading()) {
		statusSize = _data->loadOffset();
	} else if (dataLoaded()) {
		statusSize = Ui::FileStatusSizeLoaded;
	} else {
		statusSize = Ui::FileStatusSizeReady;
	}

	if (_data->isVoiceMessage()) {
		const auto state = ::Media::Player::instance()->getState(AudioMsgId::Type::Voice);
		if (state.id == AudioMsgId(_data, _realParent->fullId(), state.id.externalPlayId())
			&& !::Media::Player::IsStoppedOrStopping(state.state)) {
			if (auto voice = Get<HistoryDocumentVoice>()) {
				bool was = (voice->_playback != nullptr);
				voice->ensurePlayback(this);
				if (!was || state.position != voice->_playback->position) {
					auto prg = state.length
						? std::clamp(
							float64(state.position) / state.length,
							0.,
							1.)
						: 0.;
					if (voice->_playback->position < state.position) {
						voice->_playback->progress.start(prg);
					} else {
						voice->_playback->progress = anim::value(0., prg);
					}
					voice->_playback->position = state.position;
					voice->_playback->progressAnimation.start();
				}
				voice->_lastDurationMs = static_cast<int>((state.length * 1000LL) / state.frequency); // Bad :(
			}

			statusSize = -1 - (state.position / state.frequency);
			realDuration = (state.length / state.frequency);
			showPause = ::Media::Player::ShowPauseIcon(state.state);
		} else {
			if (auto voice = Get<HistoryDocumentVoice>()) {
				voice->checkPlaybackFinished();
			}
		}
		if (!showPause && (state.id == AudioMsgId(_data, _realParent->fullId(), state.id.externalPlayId()))) {
			showPause = ::Media::Player::instance()->isSeeking(AudioMsgId::Type::Voice);
		}
	} else if (_data->isAudioFile()) {
		const auto state = ::Media::Player::instance()->getState(AudioMsgId::Type::Song);
		if (state.id == AudioMsgId(_data, _realParent->fullId(), state.id.externalPlayId())
			&& !::Media::Player::IsStoppedOrStopping(state.state)) {
			statusSize = -1 - (state.position / state.frequency);
			realDuration = (state.length / state.frequency);
			showPause = ::Media::Player::ShowPauseIcon(state.state);
		} else {
		}
		if (!showPause && (state.id == AudioMsgId(_data, _realParent->fullId(), state.id.externalPlayId()))) {
			showPause = ::Media::Player::instance()->isSeeking(AudioMsgId::Type::Song);
		}
	}

	if (statusSize != _statusSize) {
		setStatusSize(statusSize, realDuration);
	}
	return showPause;
}

QMargins Document::bubbleMargins() const {
	if (!Has<HistoryDocumentThumbed>()) {
		return st::msgPadding;
	}
	const auto padding = st::msgFileThumbLayout.padding;
	return QMargins(padding.left(), padding.top(), padding.left(), padding.bottom());
}

QSize Document::sizeForGroupingOptimal(int maxWidth) const {
	const auto thumbed = Get<HistoryDocumentThumbed>();
	const auto &st = (thumbed ? st::msgFileThumbLayoutGrouped : st::msgFileLayoutGrouped);
	auto height = st.padding.top() + st.thumbSize + st.padding.bottom();
	if (const auto captioned = Get<HistoryDocumentCaptioned>()) {
		auto captionw = maxWidth
			- st::msgPadding.left()
			- st::msgPadding.right();
		height += captioned->_caption.countHeight(captionw);
	}
	return { maxWidth, height };
}

QSize Document::sizeForGrouping(int width) const {
	const auto thumbed = Get<HistoryDocumentThumbed>();
	const auto &st = (thumbed ? st::msgFileThumbLayoutGrouped : st::msgFileLayoutGrouped);
	auto height = st.padding.top() + st.thumbSize + st.padding.bottom();
	if (const auto captioned = Get<HistoryDocumentCaptioned>()) {
		auto captionw = width
			- st::msgPadding.left()
			- st::msgPadding.right();
		height += captioned->_caption.countHeight(captionw);
	}
	return { maxWidth(), height };
}

void Document::drawGrouped(
		Painter &p,
		const PaintContext &context,
		const QRect &geometry,
		RectParts sides,
		RectParts corners,
		float64 highlightOpacity,
		not_null<uint64*> cacheKey,
		not_null<QPixmap*> cache) const {
	p.translate(geometry.topLeft());
	draw(
		p,
		context.translated(-geometry.topLeft()),
		geometry.width(),
		LayoutMode::Grouped);
	p.translate(-geometry.topLeft());
}

TextState Document::getStateGrouped(
		const QRect &geometry,
		RectParts sides,
		QPoint point,
		StateRequest request) const {
	point -= geometry.topLeft();
	return textState(
		point,
		geometry.size(),
		request,
		LayoutMode::Grouped);
}

bool Document::voiceProgressAnimationCallback(crl::time now) {
	if (anim::Disabled()) {
		now += (2 * kAudioVoiceMsgUpdateView);
	}
	if (const auto voice = Get<HistoryDocumentVoice>()) {
		if (voice->_playback) {
			const auto dt = (now - voice->_playback->progressAnimation.started())
				/ float64(2 * kAudioVoiceMsgUpdateView);
			if (dt >= 1.) {
				voice->_playback->progressAnimation.stop();
				voice->_playback->progress.finish();
			} else {
				voice->_playback->progress.update(qMin(dt, 1.), anim::linear);
			}
			repaint();
			return (dt < 1.);
		}
	}
	return false;
}

void Document::clickHandlerPressedChanged(const ClickHandlerPtr &p, bool pressed) {
	if (auto voice = Get<HistoryDocumentVoice>()) {
		if (pressed && p == voice->_seekl && !voice->seeking()) {
			voice->startSeeking();
		} else if (!pressed && voice->seeking()) {
			const auto type = AudioMsgId::Type::Voice;
			const auto state = ::Media::Player::instance()->getState(type);
			if (state.id == AudioMsgId(_data, _realParent->fullId(), state.id.externalPlayId()) && state.length) {
				const auto currentProgress = voice->seekingCurrent();
				::Media::Player::instance()->finishSeeking(
					AudioMsgId::Type::Voice,
					currentProgress);

				voice->ensurePlayback(this);
				voice->_playback->position = 0;
				voice->_playback->progress = anim::value(currentProgress, currentProgress);
			}
			voice->stopSeeking();
		}
	}
	File::clickHandlerPressedChanged(p, pressed);
}

void Document::refreshParentId(not_null<HistoryItem*> realParent) {
	File::refreshParentId(realParent);

	const auto fullId = realParent->fullId();
	if (auto thumbed = Get<HistoryDocumentThumbed>()) {
		if (thumbed->_linksavel) {
			thumbed->_linksavel->setMessageId(fullId);
			thumbed->_linkcancell->setMessageId(fullId);
		}
	}
	if (auto voice = Get<HistoryDocumentVoice>()) {
		if (voice->_seekl) {
			voice->_seekl->setMessageId(fullId);
		}
	}
}

void Document::parentTextUpdated() {
	auto caption = (_parent->media() == this || _realParent->groupId())
		? createCaption()
		: Ui::Text::String();
	if (!caption.isEmpty()) {
		AddComponents(HistoryDocumentCaptioned::Bit());
		auto captioned = Get<HistoryDocumentCaptioned>();
		captioned->_caption = std::move(caption);
	} else {
		RemoveComponents(HistoryDocumentCaptioned::Bit());
	}
	history()->owner().requestViewResize(_parent);
}

TextWithEntities Document::getCaption() const {
	if (const auto captioned = Get<HistoryDocumentCaptioned>()) {
		return captioned->_caption.toTextWithEntities();
	}
	return TextWithEntities();
}

Ui::Text::String Document::createCaption() {
	return File::createCaption(_realParent);
}

bool DrawThumbnailAsSongCover(
		Painter &p,
		const style::color &colored,
		const std::shared_ptr<Data::DocumentMedia> &dataMedia,
		const QRect &rect,
		const bool selected) {
	if (!dataMedia) {
		return false;
	}

	auto cover = QPixmap();
	const auto scaled = [&](not_null<Image*> image) {
		const auto aspectRatio = Qt::KeepAspectRatioByExpanding;
		return image->size().scaled(rect.size(), aspectRatio);
	};
	const auto args = Images::PrepareArgs{
		.colored = &colored,
		.options = Images::Option::RoundCircle,
		.outer = rect.size(),
	};
	if (const auto normal = dataMedia->thumbnail()) {
		cover = normal->pixSingle(scaled(normal), args);
	} else if (const auto blurred = dataMedia->thumbnailInline()) {
		cover = blurred->pixSingle(scaled(blurred), args.blurred());
	} else {
		return false;
	}
	if (selected) {
		auto selectedCover = Images::Colored(
			cover.toImage(),
			p.textPalette().selectOverlay);
		cover = QPixmap::fromImage(
			std::move(selectedCover),
			Qt::ColorOnly);
	}
	p.drawPixmap(rect.topLeft(), cover);

	return true;
}

} // namespace HistoryView
