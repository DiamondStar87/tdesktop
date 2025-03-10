/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/edit_caption_box.h"

#include "api/api_editing.h"
#include "api/api_text_entities.h"
#include "apiwrap.h"
#include "base/event_filter.h"
#include "boxes/premium_limits_box.h"
#include "boxes/premium_preview_box.h"
#include "chat_helpers/emoji_suggestions_widget.h"
#include "chat_helpers/message_field.h"
#include "chat_helpers/tabbed_panel.h"
#include "chat_helpers/tabbed_selector.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "core/mime_type.h"
#include "data/data_document.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/data_premium_limits.h"
#include "data/stickers/data_stickers.h"
#include "data/stickers/data_custom_emoji.h"
#include "editor/photo_editor_layer_widget.h"
#include "history/history_drag_area.h"
#include "history/history_item.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "mtproto/mtproto_config.h"
#include "platform/platform_specific.h"
#include "storage/localimageloader.h" // SendMediaType
#include "storage/storage_media_prepare.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/attach/attach_item_single_file_preview.h"
#include "ui/chat/attach/attach_item_single_media_preview.h"
#include "ui/chat/attach/attach_single_file_preview.h"
#include "ui/chat/attach/attach_single_media_preview.h"
#include "ui/controls/emoji_button.h"
#include "ui/effects/scroll_content_shadow.h"
#include "ui/image/image.h"
#include "ui/toast/toast.h"
#include "ui/ui_utility.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_layers.h"

#include <QtCore/QMimeData>

namespace {

auto ListFromMimeData(not_null<const QMimeData*> data, bool premium) {
	using Error = Ui::PreparedList::Error;
	auto result = data->hasUrls()
		? Storage::PrepareMediaList(
			// When we edit media, we need only 1 file.
			base::GetMimeUrls(data).mid(0, 1),
			st::sendMediaPreviewSize,
			premium)
		: Ui::PreparedList(Error::EmptyFile, QString());
	if (result.error == Error::None) {
		return result;
	} else if (data->hasImage()) {
		auto image = qvariant_cast<QImage>(data->imageData());
		if (!image.isNull()) {
			return Storage::PrepareMediaFromImage(
				std::move(image),
				QByteArray(),
				st::sendMediaPreviewSize);
		}
	}
	return result;
}

Ui::AlbumType ComputeAlbumType(not_null<HistoryItem*> item) {
	if (item->groupId().empty()) {
		return Ui::AlbumType();
	}
	const auto media = item->media();

	if (media->photo()) {
		return Ui::AlbumType::PhotoVideo;
	} else if (const auto document = media->document()) {
		if (document->isVideoFile()) {
			return Ui::AlbumType::PhotoVideo;
		} else if (document->isSong()) {
			return Ui::AlbumType::Music;
		} else {
			return Ui::AlbumType::File;
		}
	}
	return Ui::AlbumType();
}

bool CanBeCompressed(Ui::AlbumType type) {
	return (type == Ui::AlbumType::None)
		|| (type == Ui::AlbumType::PhotoVideo);
}

} // namespace

EditCaptionBox::EditCaptionBox(
	QWidget*,
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item)
: _controller(controller)
, _historyItem(item)
, _isAllowedEditMedia(item->media()
	? item->media()->allowsEditMedia()
	: false)
, _albumType(ComputeAlbumType(item))
, _controls(base::make_unique_q<Ui::VerticalLayout>(this))
, _scroll(base::make_unique_q<Ui::ScrollArea>(this, st::boxScroll))
, _field(base::make_unique_q<Ui::InputField>(
	this,
	st::confirmCaptionArea,
	Ui::InputField::Mode::MultiLine,
	tr::lng_photo_caption()))
, _emojiToggle(base::make_unique_q<Ui::EmojiButton>(
	this,
	st::boxAttachEmoji)) {
	Expects(item->media() != nullptr);
	Expects(item->media()->allowsEditCaption());

	_controller->session().data().itemRemoved(
		_historyItem->fullId()
	) | rpl::start_with_next([=] {
		closeBox();
	}, lifetime());
}

EditCaptionBox::~EditCaptionBox() = default;

void EditCaptionBox::prepare() {
	addButton(tr::lng_settings_save(), [=] { save(); });
	addButton(tr::lng_cancel(), [=] { closeBox(); });

	updateBoxSize();

	setupField();
	setupEmojiPanel();
	setInitialText();

	rebuildPreview();
	setupEditEventHandler();
	SetupShadowsToScrollContent(this, _scroll, _contentHeight.events());

	setupControls();
	setupPhotoEditorEventHandler();

	setupDragArea();

	captionResized();
}

void EditCaptionBox::rebuildPreview() {
	const auto gifPaused = [controller = _controller] {
		return controller->isGifPausedAtLeastFor(
			Window::GifPauseReason::Layer);
	};

	if (_preparedList.files.empty()) {
		const auto media = _historyItem->media();
		const auto photo = media->photo();
		const auto document = media->document();
		if (photo || document->isVideoFile() || document->isAnimation()) {
			_isPhoto = (photo != nullptr);
			const auto media = Ui::CreateChild<Ui::ItemSingleMediaPreview>(
				this,
				gifPaused,
				_historyItem,
				Ui::AttachControls::Type::EditOnly);
			_photoMedia = media->sharedPhotoMedia();
			_content.reset(media);
		} else {
			_isPhoto = false;
			_content.reset(Ui::CreateChild<Ui::ItemSingleFilePreview>(
				this,
				_historyItem,
				Ui::AttachControls::Type::EditOnly));
		}
	} else {
		const auto &file = _preparedList.files.front();

		const auto media = Ui::SingleMediaPreview::Create(
			this,
			gifPaused,
			file,
			Ui::AttachControls::Type::EditOnly);
		if (media) {
			_isPhoto = media->isPhoto();
			_content.reset(media);
		} else {
			_isPhoto = false;
			_content.reset(Ui::CreateChild<Ui::SingleFilePreview>(
				this,
				file,
				Ui::AttachControls::Type::EditOnly));
		}
	}
	Assert(_content != nullptr);

	rpl::combine(
		_content->heightValue(),
		_footerHeight.value(),
		rpl::single(st::boxPhotoPadding.top()),
		rpl::mappers::_1 + rpl::mappers::_2 + rpl::mappers::_3
	) | rpl::start_with_next([=](int height) {
		setDimensions(
			st::boxWideWidth,
			std::min(st::sendMediaPreviewHeightMax, height),
			true);
	}, _content->lifetime());

	_content->editRequests(
	) | rpl::start_to_stream(_editMediaClicks, _content->lifetime());

	_content->modifyRequests(
	) | rpl::start_to_stream(_photoEditorOpens, _content->lifetime());

	_content->heightValue(
	) | rpl::start_to_stream(_contentHeight, _content->lifetime());

	_scroll->setOwnedWidget(
		object_ptr<Ui::RpWidget>::fromRaw(_content.get()));

	_previewRebuilds.fire({});

	captionResized();
}

void EditCaptionBox::setupField() {
	const auto peer = _historyItem->history()->peer;
	const auto allow = [=](const auto&) {
		return Data::AllowEmojiWithoutPremium(peer);
	};
	InitMessageFieldHandlers(
		_controller,
		_field.get(),
		Window::GifPauseReason::Layer,
		allow);
	Ui::Emoji::SuggestionsController::Init(
		getDelegate()->outerContainer(),
		_field,
		&_controller->session(),
		{ .suggestCustomEmoji = true, .allowCustomWithoutPremium = allow });

	_field->setSubmitSettings(
		Core::App().settings().sendSubmitWay());
	_field->setMaxHeight(st::confirmCaptionArea.heightMax);

	connect(_field, &Ui::InputField::submitted, [=] { save(); });
	connect(_field, &Ui::InputField::cancelled, [=] { closeBox(); });
	connect(_field, &Ui::InputField::resized, [=] { captionResized(); });
	_field->setMimeDataHook([=](
			not_null<const QMimeData*> data,
			Ui::InputField::MimeAction action) {
		if (action == Ui::InputField::MimeAction::Check) {
			if (!data->hasText() && !_isAllowedEditMedia) {
				return false;
			} else if (Storage::ValidateEditMediaDragData(data, _albumType)) {
				return true;
			}
			return data->hasText();
		} else if (action == Ui::InputField::MimeAction::Insert) {
			return fileFromClipboard(data);
		}
		Unexpected("Action in MimeData hook.");
	});
}

void EditCaptionBox::setInitialText() {
	_field->setTextWithTags(
		PrepareEditText(_historyItem),
		Ui::InputField::HistoryAction::Clear);
	auto cursor = _field->textCursor();
	cursor.movePosition(QTextCursor::End);
	_field->setTextCursor(cursor);
}

void EditCaptionBox::setupControls() {
	auto hintLabelToggleOn = _previewRebuilds.events_starting_with(
		{}
	) | rpl::map([=] {
		return _controller->session().settings().photoEditorHintShown()
			? _isPhoto
			: false;
	});

	_controls->add(object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
		this,
		object_ptr<Ui::FlatLabel>(
			this,
			tr::lng_edit_photo_editor_hint(tr::now),
			st::editMediaHintLabel),
		st::editMediaLabelMargins)
	)->toggleOn(std::move(hintLabelToggleOn), anim::type::instant);

	_controls->add(object_ptr<Ui::SlideWrap<Ui::Checkbox>>(
		this,
		object_ptr<Ui::Checkbox>(
			this,
			tr::lng_send_compressed(tr::now),
			true,
			st::defaultBoxCheckbox),
		st::editMediaCheckboxMargins)
	)->toggleOn(
		_previewRebuilds.events_starting_with({}) | rpl::map([=] {
			return _isPhoto
				&& CanBeCompressed(_albumType)
				&& !_preparedList.files.empty();
		}),
		anim::type::instant
	)->entity()->checkedChanges(
	) | rpl::start_with_next([&](bool checked) {
		_asFile = !checked;
	}, _controls->lifetime());

	_controls->resizeToWidth(st::sendMediaPreviewSize);
}

void EditCaptionBox::setupEditEventHandler() {
	const auto toastParent = Ui::BoxShow(this).toastParent();
	const auto callback = [=](FileDialog::OpenResult &&result) {
		auto showError = [toastParent](tr::phrase<> t) {
			Ui::Toast::Show(toastParent, t(tr::now));
		};

		const auto checkResult = [=](const Ui::PreparedList &list) {
			if (list.files.size() != 1) {
				return false;
			}
			const auto &file = list.files.front();
			const auto mime = file.information->filemime;
			if (Core::IsMimeSticker(mime)) {
				showError(tr::lng_edit_media_invalid_file);
				return false;
			} else if (_albumType != Ui::AlbumType::None
				&& !file.canBeInAlbumType(_albumType)) {
				showError(tr::lng_edit_media_album_error);
				return false;
			}
			return true;
		};
		const auto premium = _controller->session().premium();
		auto list = Storage::PreparedFileFromFilesDialog(
			std::move(result),
			checkResult,
			showError,
			st::sendMediaPreviewSize,
			premium);

		if (list) {
			setPreparedList(std::move(*list));
		}
	};

	const auto buttonCallback = [=] {
		const auto filters = (_albumType == Ui::AlbumType::PhotoVideo)
			? FileDialog::PhotoVideoFilesFilter()
			: FileDialog::AllFilesFilter();
		FileDialog::GetOpenPath(
			this,
			tr::lng_choose_file(tr::now),
			filters,
			crl::guard(this, callback));
	};

	_editMediaClicks.events(
	) | rpl::start_with_next(
		buttonCallback,
		lifetime());
}

void EditCaptionBox::setupPhotoEditorEventHandler() {
	const auto openedOnce = lifetime().make_state<bool>(false);
	_photoEditorOpens.events(
	) | rpl::start_with_next([=, controller = _controller] {
		const auto increment = [=] {
			if (*openedOnce) {
				return;
			}
			*openedOnce = true;
			controller->session().settings().incrementPhotoEditorHintShown();
			controller->session().saveSettings();
		};
		const auto clearError = [=] {
			_error = QString();
			update();
		};
		const auto previewWidth = st::sendMediaPreviewSize;
		if (!_preparedList.files.empty()) {
			increment();
			clearError();
			Editor::OpenWithPreparedFile(
				this,
				controller,
				&_preparedList.files.front(),
				previewWidth,
				[=] { rebuildPreview(); });
		} else if (_photoMedia) {
			const auto large = _photoMedia->image(Data::PhotoSize::Large);
			if (!large) {
				return;
			}
			increment();
			clearError();
			auto callback = [=](const Editor::PhotoModifications &mods) {
				if (!mods || !_photoMedia) {
					return;
				}
				const auto large = _photoMedia->image(Data::PhotoSize::Large);
				if (!large) {
					return;
				}
				auto copy = large->original();
				_preparedList = Storage::PrepareMediaFromImage(
					std::move(copy),
					QByteArray(),
					previewWidth);

				using ImageInfo = Ui::PreparedFileInformation::Image;
				auto &file = _preparedList.files.front();
				const auto image = std::get_if<ImageInfo>(
					&file.information->media);

				image->modifications = mods;
				Storage::UpdateImageDetails(file, previewWidth);
				rebuildPreview();
			};
			const auto fileImage = std::make_shared<Image>(*large);
			controller->showLayer(
				std::make_unique<Editor::LayerWidget>(
					this,
					&controller->window(),
					fileImage,
					Editor::PhotoModifications(),
					std::move(callback)),
				Ui::LayerOption::KeepOther);
		}
	}, lifetime());
}

void EditCaptionBox::setupDragArea() {
	auto enterFilter = [=](not_null<const QMimeData*> data) {
		return !_isAllowedEditMedia
			? false
			: Storage::ValidateEditMediaDragData(data, _albumType);
	};
	// Avoid both drag areas appearing at one time.
	auto computeState = [=](const QMimeData *data) {
		const auto state = Storage::ComputeMimeDataState(data);
		return (state == Storage::MimeDataState::PhotoFiles)
			? Storage::MimeDataState::Image
			: state;
	};
	const auto areas = DragArea::SetupDragAreaToContainer(
		this,
		std::move(enterFilter),
		[=](bool f) { _field->setAcceptDrops(f); },
		nullptr,
		std::move(computeState));

	const auto droppedCallback = [=](bool compress) {
		return [=](const QMimeData *data) {
			fileFromClipboard(data);
			Window::ActivateWindow(_controller);
		};
	};
	areas.document->setDroppedCallback(droppedCallback(false));
	areas.photo->setDroppedCallback(droppedCallback(true));
}

void EditCaptionBox::setupEmojiPanel() {
	const auto container = getDelegate()->outerContainer();
	using Selector = ChatHelpers::TabbedSelector;
	_emojiPanel = base::make_unique_q<ChatHelpers::TabbedPanel>(
		container,
		_controller,
		object_ptr<Selector>(
			nullptr,
			_controller,
			Window::GifPauseReason::Layer,
			Selector::Mode::EmojiOnly));
	_emojiPanel->setDesiredHeightValues(
		1.,
		st::emojiPanMinHeight / 2,
		st::emojiPanMinHeight);
	_emojiPanel->hide();
	_emojiPanel->selector()->setCurrentPeer(_historyItem->history()->peer);
	_emojiPanel->selector()->emojiChosen(
	) | rpl::start_with_next([=](ChatHelpers::EmojiChosen data) {
		Ui::InsertEmojiAtCursor(_field->textCursor(), data.emoji);
	}, lifetime());
	_emojiPanel->selector()->customEmojiChosen(
	) | rpl::start_with_next([=](ChatHelpers::FileChosen data) {
		const auto info = data.document->sticker();
		if (info
			&& info->setType == Data::StickersType::Emoji
			&& !_controller->session().premium()) {
			ShowPremiumPreviewBox(
				_controller,
				PremiumPreview::AnimatedEmoji);
		} else {
			Data::InsertCustomEmoji(_field.get(), data.document);
		}
	}, lifetime());

	const auto filterCallback = [=](not_null<QEvent*> event) {
		emojiFilterForGeometry(event);
		return base::EventFilterResult::Continue;
	};
	_emojiFilter.reset(base::install_event_filter(container, filterCallback));

	_emojiToggle->installEventFilter(_emojiPanel);
	_emojiToggle->addClickHandler([=] {
		_emojiPanel->toggleAnimated();
	});
}

void EditCaptionBox::emojiFilterForGeometry(not_null<QEvent*> event) {
	const auto type = event->type();
	if (type == QEvent::Move || type == QEvent::Resize) {
		// updateEmojiPanelGeometry uses not only container geometry, but
		// also container children geometries that will be updated later.
		crl::on_main(this, [=] { updateEmojiPanelGeometry(); });
	}
}

void EditCaptionBox::updateEmojiPanelGeometry() {
	const auto parent = _emojiPanel->parentWidget();
	const auto global = _emojiToggle->mapToGlobal({ 0, 0 });
	const auto local = parent->mapFromGlobal(global);
	_emojiPanel->moveBottomRight(
		local.y(),
		local.x() + _emojiToggle->width() * 3);
}

bool EditCaptionBox::fileFromClipboard(not_null<const QMimeData*> data) {
	const auto premium = _controller->session().premium();
	return setPreparedList(ListFromMimeData(data, premium));
}

bool EditCaptionBox::setPreparedList(Ui::PreparedList &&list) {
	if (!_isAllowedEditMedia) {
		return false;
	}
	using Error = Ui::PreparedList::Error;
	if (list.error != Error::None || list.files.empty()) {
		return false;
	}
	auto file = &list.files.front();
	const auto invalidForAlbum = (_albumType != Ui::AlbumType::None)
		&& !file->canBeInAlbumType(_albumType);
	if (_albumType == Ui::AlbumType::PhotoVideo) {
		using Video = Ui::PreparedFileInformation::Video;
		if (const auto video = std::get_if<Video>(
				&file->information->media)) {
			video->isGifv = false;
		}
	}
	if (invalidForAlbum) {
		Ui::Toast::Show(
			Ui::BoxShow(this).toastParent(),
			tr::lng_edit_media_album_error(tr::now));
		return false;
	}
	_preparedList = std::move(list);
	rebuildPreview();
	return true;
}

void EditCaptionBox::captionResized() {
	updateBoxSize();
	resizeEvent(0);
	updateEmojiPanelGeometry();
	update();
}

void EditCaptionBox::updateBoxSize() {
	auto footerHeight = 0;
	footerHeight += st::normalFont->height + errorTopSkip();
	if (_field) {
		footerHeight += st::boxPhotoCaptionSkip + _field->height();
	}
	if (_controls && !_controls->isHidden()) {
		footerHeight += _controls->heightNoMargins();
	}
	_footerHeight = footerHeight;
}

int EditCaptionBox::errorTopSkip() const {
	return (st::defaultBox.buttonPadding.top() / 2);
}

void EditCaptionBox::paintEvent(QPaintEvent *e) {
	BoxContent::paintEvent(e);

	Painter p(this);

	if (!_error.isEmpty()) {
		p.setFont(st::normalFont);
		p.setPen(st::boxTextFgError);
		p.drawTextLeft(
			_field->x(),
			_field->y() + _field->height() + errorTopSkip(),
			width(),
			_error);
	}

}

void EditCaptionBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);

	const auto errorHeight = st::normalFont->height + errorTopSkip();
	auto bottom = height();
	{
		const auto resultScrollHeight = bottom
			- _field->height()
			- st::boxPhotoCaptionSkip
			- (_controls->isHidden() ? 0 : _controls->heightNoMargins())
			- st::boxPhotoPadding.top()
			- errorHeight;
		const auto minThumbH = st::sendBoxAlbumGroupSize.height()
			+ st::sendBoxAlbumGroupSkipTop * 2;
		const auto diff = resultScrollHeight - minThumbH;
		if (diff < 0) {
			bottom -= diff;
		}
	}

	bottom -= errorHeight;
	_field->resize(st::sendMediaPreviewSize, _field->height());
	_field->moveToLeft(
		st::boxPhotoPadding.left(),
		bottom - _field->height());
	bottom -= st::boxPhotoCaptionSkip + _field->height();

	_emojiToggle->moveToLeft(
		(st::boxPhotoPadding.left()
			+ st::sendMediaPreviewSize
			- _emojiToggle->width()),
		_field->y() + st::boxAttachEmojiTop);
	_emojiToggle->update();

	if (!_controls->isHidden()) {
		_controls->resizeToWidth(width());
		_controls->moveToLeft(
			st::boxPhotoPadding.left(),
			bottom - _controls->heightNoMargins());
		bottom -= _controls->heightNoMargins();
	}
	_scroll->resize(width(), bottom - st::boxPhotoPadding.top());
	_scroll->move(0, st::boxPhotoPadding.top());

	if (_content) {
		_content->resize(_scroll->width(), _content->height());
	}
}

void EditCaptionBox::setInnerFocus() {
	_field->setFocusFast();
}

bool EditCaptionBox::validateLength(const QString &text) const {
	const auto session = &_controller->session();
	const auto limit = Data::PremiumLimits(session).captionLengthCurrent();
	const auto remove = int(text.size()) - limit;
	if (remove <= 0) {
		return true;
	}
	_controller->show(Box(CaptionLimitReachedBox, session, remove));
	return false;
}

void EditCaptionBox::save() {
	if (_saveRequestId) {
		return;
	}

	const auto item = _controller->session().data().message(
		_historyItem->fullId());
	if (!item) {
		_error = tr::lng_edit_deleted(tr::now);
		update();
		return;
	}

	const auto textWithTags = _field->getTextWithAppliedMarkdown();
	if (!validateLength(textWithTags.text)) {
		return;
	}
	const auto sending = TextWithEntities{
		textWithTags.text,
		TextUtilities::ConvertTextTagsToEntities(textWithTags.tags)
	};

	auto options = Api::SendOptions();
	options.scheduled = item->isScheduled() ? item->date() : 0;

	if (!_preparedList.files.empty()) {
		if ((_albumType != Ui::AlbumType::None)
				&& !_preparedList.files.front().canBeInAlbumType(
					_albumType)) {
			_error = tr::lng_edit_media_album_error(tr::now);
			update();
			return;
		}
		auto action = Api::SendAction(item->history(), options);
		action.replaceMediaOf = item->fullId().msg;

		Storage::ApplyModifications(_preparedList);

		_controller->session().api().editMedia(
			std::move(_preparedList),
			(!_asFile && _isPhoto && CanBeCompressed(_albumType))
				? SendMediaType::Photo
				: SendMediaType::File,
			_field->getTextWithAppliedMarkdown(),
			action);
		closeBox();
		return;
	}

	const auto done = crl::guard(this, [=] {
		_saveRequestId = 0;
		closeBox();
	});

	const auto fail = crl::guard(this, [=](const QString &error) {
		_saveRequestId = 0;
		if (ranges::contains(Api::kDefaultEditMessagesErrors, error)) {
			_error = tr::lng_edit_error(tr::now);
			update();
		} else if (error == u"MESSAGE_NOT_MODIFIED"_q) {
			closeBox();
		} else if (error == u"MESSAGE_EMPTY"_q) {
			_field->setFocus();
			_field->showError();
			update();
		} else {
			_error = tr::lng_edit_error(tr::now);
			update();
		}
	});

	lifetime().add([=] {
		if (_saveRequestId) {
			auto &session = _controller->session();
			session.api().request(base::take(_saveRequestId)).cancel();
		}
	});

	_saveRequestId = Api::EditCaption(item, sending, options, done, fail);
}

void EditCaptionBox::keyPressEvent(QKeyEvent *e) {
	const auto ctrl = e->modifiers().testFlag(Qt::ControlModifier);
	if ((e->key() == Qt::Key_E) && ctrl) {
		_photoEditorOpens.fire({});
	} else if ((e->key() == Qt::Key_O) && ctrl) {
		_editMediaClicks.fire({});
	} else {
		e->ignore();
	}
}
