#pragma once

// Which screen a tab is showing. Lives in FolderNavigationService::Location so the
// back-stack can return to a screen that isn't a folder (FAVOURITES_VIEW_SPEC.md 3.1).
enum class ViewKind
{
    CloudDrive,
    Favourites,
    Rubbish,
    Recents,
    SharedLinks,
};
