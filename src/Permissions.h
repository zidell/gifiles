#pragma once

class QWidget;

// macOS Full Disk Access. Without it macOS asks separately for Desktop, Documents, Downloads,
// removable and network volumes; with it, never.
namespace Permissions {

bool hasFullDiskAccess();
// Explains the permission and opens System Settings at the right pane. With force=false it is
// skipped when access is already granted or the user chose "다시 묻지 않기".
void showDialog(QWidget *parent, bool force);

} // namespace Permissions
