// Copyright (c) .NET Foundation. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include "precomp.h"

extern "C" HRESULT Initialize(int argc, wchar_t* argv[])
{
    HRESULT hr = S_OK;

    // We're not going to do any clever parsing. This is intended to be called from
    // the standalone bundle only and there will only be a fixed set of parameters:
    // 1. The path of the log file, created by the bundle.
    // 2. The full SDK version, e.g. 6.0.105 or 6.0.398-preview19
    // 3. Target platform to search under the registry key to locate installed SDKs.
    if (4 != argc)
    {
        return HRESULT_FROM_WIN32(ERROR_INVALID_COMMAND_LINE);
    }

    LogInitialize(::GetModuleHandleW(NULL));

#ifdef _DEBUG
    LogSetLevel(REPORT_DEBUG, FALSE);
#else
    LogSetLevel(REPORT_VERBOSE, FALSE); // FALSE means don't write an additional text line to the log saying the level changed
#endif

    hr = LogOpen(NULL, argv[1], NULL, NULL, FALSE, TRUE, NULL);
    ExitOnFailure(hr, "Failed to create log file.");

    hr = RegInitialize();
    ExitOnFailure(hr, "Failed to initialize the registry.");

    hr = WiuInitialize();
    ExitOnFailure(hr, "Failed to initialize Windows Installer.");

LExit:
    return hr;
}

extern "C" HRESULT RemoveDependent(LPWSTR sczDependent, BOOL * pbRestartRequired)
{
    HRESULT hr = S_OK;
    HKEY hkInstallerDependenciesKey = NULL;
    HKEY hkProviderKey = NULL;
    HKEY hkDependentsKey = NULL;
    LPWSTR sczProviderKey = NULL;
    LPWSTR sczDependentsKey = NULL;
    LPWSTR sczProductId = NULL;
    LPWSTR sczProductName = NULL;
    DWORD cSubKeys = 0;
    DWORD dwExitCode = 0;
    WIU_RESTART restart = WIU_RESTART_NONE;

    // Optional workloads are always per-machine installs, so we don't need to check HKCU. 
    hr = RegOpen(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\Installer\\Dependencies", KEY_READ, &hkInstallerDependenciesKey);
    ExitOnFailure(hr, "Failed to read installer dependencies key.");

    // This has to be an exhaustive search as we're not looking for a specific provider key, but for a specific dependent
    // that could be registered against any provider key.
    for (DWORD dwIndex = 0;; ++dwIndex)
    {
        // Get the next provider key name
        hr = RegKeyEnum(hkInstallerDependenciesKey, dwIndex, &sczProviderKey);

        if (E_NOMOREITEMS == hr)
        {
            hr = S_OK;
            break;
        }

        ExitOnFailure(hr, "Failed to enumerate installer dependency provider keys.");
        LogStringLine(REPORT_STANDARD, "Processing provider key: %ls", sczProviderKey);

        hr = RegOpen(hkInstallerDependenciesKey, sczProviderKey, KEY_READ, &hkProviderKey);
        ExitOnFailure(hr, "Unable to open provider key.");

        // Open the dependents key with write permissions so we can modify it if it matches
        // the target dependent value.
        hr = RegOpen(hkProviderKey, L"Dependents", KEY_READ | KEY_WRITE, &hkDependentsKey);
        if (E_FILENOTFOUND == hr)
        {
            // Providers can sometimes become orphaned during uninstalls. If there's no Dependents subkey, we just
            // release the handle and continue to the next provider key.
            hr = S_OK;
            ReleaseRegKey(hkProviderKey);

            continue;
        }

        ExitOnFailure(hr, "Unable to open dependents key.");

        // Enumerate over all the dependent keys
        for (DWORD dwDependentsKeyIndex = 0;; ++dwDependentsKeyIndex)
        {
            hr = RegKeyEnum(hkDependentsKey, dwDependentsKeyIndex, &sczDependentsKey);

            if (E_NOMOREITEMS == hr)
            {
                hr = S_OK;
                break;
            }

            ExitOnFailure(hr, "Failed to read provider's dependent key.");

            if (CSTR_EQUAL == ::CompareStringW(LOCALE_INVARIANT, 0, sczDependentsKey, -1, sczDependent, -1))
            {
                LogStringLine(REPORT_STANDARD, "  Dependent match found: %ls", sczDependentsKey);

                hr = RegDelete(hkDependentsKey, sczDependent, REG_KEY_DEFAULT, TRUE);
                ExitOnFailure(hr, "Failed to delete dependent \"%ls\"", sczDependent);
                LogStringLine(REPORT_STANDARD, "  Dependent deleted");
                // Reset the index since we're deleting keys while enumerating
                dwDependentsKeyIndex = dwDependentsKeyIndex > 1 ? dwDependentsKeyIndex-- : 0;

                // Check if there are any subkeys remaining under the dependents key. If not, we
                // can uninstall the MSI. We'll recheck the key again in case the MSI fails to clean up the 
                // provider key to make sure we don't have orphaned keys.
                hr = RegQueryKey(hkDependentsKey, &cSubKeys, NULL);
                ExitOnFailure(hr, "Failed to query dependents key.");

                LogStringLine(REPORT_STANDARD, "  Remaining dependents: %i", cSubKeys);

                if (0 == cSubKeys)
                {
                    // This was the final dependent, so now we can remove the installation if the provider wasn't corrupted and
                    // still contains the product ID.
                    hr = RegReadString(hkProviderKey, NULL, &sczProductId);

                    if (E_FILENOTFOUND == hr)
                    {
                        LogStringLine(REPORT_STANDARD, "  No product ID found, provider key: %ls", sczProviderKey);
                        hr = S_OK;
                        break;
                    }
                    else
                    {
                        ExitOnFailure(hr, "Failed to read product ID.");
                    }

                    // Let's make sure the product is actually installed. The provider key for an MSI typically
                    // stores the ProductCode, DisplayName, and Version, but by calling into MsiGetProductInfo,
                    // we're doing an implicit detect and getting a property back.
                    hr = WiuGetProductInfo(sczProductId, L"ProductName", &sczProductName);
                    if (SUCCEEDED(hr))
                    {
                        // The provider key *should* have the ProductName and ProductVersion properties, but since
                        // we know it's installed, we just query the installer service. 
                        MsiSetInternalUI(INSTALLUILEVEL_NONE, NULL);
                        hr = WiuConfigureProductEx(sczProductId, INSTALLLEVEL_DEFAULT, INSTALLSTATE_ABSENT, L"MSIFASTINSTALL=7 IGNOREDEPENDENCIES=ALL REBOOT=ReallySuppress", &restart);
                        LogStringLine(REPORT_STANDARD, "  Uninstall of \"%ls\" (%ls%) exited with 0x%.8x", sczProductName, sczProductId, hr);

                        // Flag any reboot since we need to return that to the bundle.
                        if (WIU_RESTART_INITIATED == restart || WIU_RESTART_REQUIRED == restart)
                        {
                            LogStringLine(REPORT_STANDARD, "  Reboot requested, deferring.");
                            *pbRestartRequired = TRUE;
                        }

                        // Reset potential failures so we can continue to remove as many dependents as possible.
                        hr = S_OK;
                    }
                    else if (HRESULT_FROM_WIN32(ERROR_UNKNOWN_PRODUCT) == hr || HRESULT_FROM_WIN32(ERROR_UNKNOWN_PROPERTY) == hr)
                    {
                        // Possibly a corrupted provider key that wasn't cleaned up. We'll just ignore it.
                        LogStringLine(REPORT_STANDARD, "  Product is not installed, ProductCode:%ls, result: 0x%.8x", sczProductId, hr);
                        hr = S_OK;
                    }
                }
            }
        }

        ReleaseRegKey(hkDependentsKey);
        ReleaseRegKey(hkProviderKey);
    }

LExit:
    ReleaseStr(sczProductName);
    ReleaseStr(sczProductId);
    ReleaseStr(sczProviderKey);
    ReleaseStr(sczDependentsKey);
    ReleaseRegKey(hkDependentsKey);
    ReleaseRegKey(hkProviderKey);
    ReleaseRegKey(hkInstallerDependenciesKey);
    return hr;
}

extern "C" HRESULT ParseSdkVersion(LPWSTR sczSdkVersion, INT * piMajor, INT * piMinor, INT * piFeatureBand)
{
    HRESULT hr = S_OK;
    UINT cVersionParts = 0;
    LPWSTR* rgsczVersionParts = NULL;
    int iPatch = 0;

    hr = StrSplitAllocArray(&rgsczVersionParts, &cVersionParts, sczSdkVersion, L".");
    ExitOnFailure(hr, "Failed to split version.");

    // We only care about the major.minor.patch values
    // to convert to a feature band. If we don't have at least
    // all 3 parts, we'll ignore the value.
    if (3 > cVersionParts)
    {
        ExitOnFailure(E_INVALIDARG, "Invalid SDK version: %ls %li", sczSdkVersion, cVersionParts);
    }

    hr = StrStringToInt32(rgsczVersionParts[0], 0, piMajor);
    ExitOnFailure(hr, "Invalid major version.");
    hr = StrStringToInt32(rgsczVersionParts[1], 0, piMinor);
    ExitOnFailure(hr, "Invalid minor version.");

    // If this is a valid SDK version the 'patch' should be a 3 digit field
    // containing the feature band and patch level, e.g. 100 or 207. We 
    // can discard any prerelease labels from the semantic version.
    hr = StrStringToInt32(rgsczVersionParts[2], 3, &iPatch);
    ExitOnFailure(hr, "Invalid patch version.");

    if (100 > iPatch)
    {
        hr = E_INVALIDARG;
        ExitOnFailure(hr, "Invalid SDK feature band and patch level.");
    }

    *piFeatureBand = iPatch - (iPatch % 100);

LExit:
    ReleaseStrArray(rgsczVersionParts, cVersionParts);
    return hr;
}

extern "C" HRESULT DetectSdk(LPWSTR sczSdkFeatureBandVersion, LPWSTR sczArchitecture, BOOL * pbInstalled)
{
    HRESULT hr = S_OK;
    HKEY hkInstalledSdkVersionsKey = NULL;
    LPWSTR sczInstalledSdkVersionsKeyName = NULL;
    LPWSTR sczSdkVersion = NULL;
    DWORD dwSdkVersionValueType = 0;
    int iInstalledMajor = 0;
    int iInstalledMinor = 0;
    int iInstalledFeatureBand = 0;
    int iExpectedMajor = 0;
    int iExpectedMinor = 0;
    int iExpectedFeatureBand = 0;

    hr = ParseSdkVersion(sczSdkFeatureBandVersion, &iExpectedMajor, &iExpectedMinor, &iExpectedFeatureBand);

    LogStringLine(REPORT_STANDARD, "Detecting installed SDK versions for %ls", sczSdkFeatureBandVersion);

    // Scan the registry to see if any SDK matching the feature band we're trying to
    // clean up is still installed. All the installation keys reside in the 32-bit hive.   
    hr = StrAllocConcatFormatted(&sczInstalledSdkVersionsKeyName, L"SOFTWARE\\WOW6432Node\\dotnet\\Setup\\InstalledVersions\\%ls\\sdk", sczArchitecture);
    ExitOnFailure(hr, "Failed to allocate string for installed SDK versions.");

    LogStringLine(REPORT_STANDARD, "Scanning %ls", sczInstalledSdkVersionsKeyName);

    hr = RegOpen(HKEY_LOCAL_MACHINE, sczInstalledSdkVersionsKeyName, KEY_READ, &hkInstalledSdkVersionsKey);
    ExitOnFailure(hr, "Failed to read installed versions key.");

    for (DWORD dwSdkVersionsValueIndex = 0;; ++dwSdkVersionsValueIndex)
    {
        hr = RegValueEnum(hkInstalledSdkVersionsKey, dwSdkVersionsValueIndex, &sczSdkVersion, &dwSdkVersionValueType);

        if (E_NOMOREITEMS == hr)
        {
            hr = S_OK;
            break;
        }

        ExitOnFailure(hr, "Failed to read SDK version values from registry.");

        hr = ParseSdkVersion(sczSdkVersion, &iInstalledMajor, &iInstalledMinor, &iInstalledFeatureBand);
        ExitOnFailure(hr, "Failed to parse %ls", sczSdkVersion);

        LogStringLine(REPORT_STANDARD, "SDK version detected: %ls, mapping to %li.%li.%li.", sczSdkVersion, iInstalledMajor, iInstalledMinor, iInstalledFeatureBand);

        // Bail out on the first match.
        if ((iInstalledMajor == iExpectedMajor) && (iInstalledMinor == iExpectedMinor) && (iInstalledFeatureBand == iExpectedFeatureBand))
        {
            *pbInstalled = TRUE;
            break;
        }
    }

LExit:
    ReleaseRegKey(hkInstalledSdkVersionsKey);
    ReleaseStr(sczInstalledSdkVersionsKeyName);
    ReleaseStr(sczSdkVersion);
    return hr;
}

int wmain(int argc, wchar_t* argv[])
{
    HRESULT hr = S_OK;
    DWORD dwExitCode = 0;
    LPWSTR sczDependent = NULL;
    LPWSTR sczFeatureBandVersion = NULL;
    BOOL bRestartRequired = FALSE;
    BOOL bSdkFeatureBandInstalled = FALSE;
    int iMajor = 0;
    int iMinor = 0;
    int iFeatureBand = 0;

    hr = ::Initialize(argc, argv);
    ExitOnFailure(hr, "Failed to initialize.");

    // Convert the full SDK version to a feature band version
    hr = ParseSdkVersion(argv[2], &iMajor, &iMinor, &iFeatureBand);
    ExitOnFailure(hr, "Failed to parse version, %ls.", argv[2]);

    hr = StrAllocConcatFormatted(&sczFeatureBandVersion, L"%li.%li.%li", iMajor, iMinor, iFeatureBand);
    ExitOnFailure(hr, "Failed to create feature band version.");

    // Create the dependent value, e.g., Microsoft.NET.Sdk,6.0.300,arm64
    hr = StrAllocConcatFormatted(&sczDependent, L"Microsoft.NET.Sdk,%ls,%ls", sczFeatureBandVersion, argv[3]);
    ExitOnFailure(hr, "Failed to create dependent.");
    LogStringLine(REPORT_STANDARD, "Setting target dependent to %ls.", sczDependent);

    hr = ::DetectSdk(sczFeatureBandVersion, argv[3], &bSdkFeatureBandInstalled);
    ExitOnFailure(hr, "Failed to detect installed SDKs.");

    if (bSdkFeatureBandInstalled)
    {
        goto LExit;
    }

    LogStringLine(REPORT_STANDARD, "SDK with feature band %ls could not be found.", sczFeatureBandVersion);

    hr = ::RemoveDependent(sczDependent, &bRestartRequired);
    ExitOnFailure(hr, "Failed to remove dependent \"%ls\".", sczDependent);

    if (bRestartRequired)
    {
        dwExitCode = ERROR_SUCCESS_REBOOT_REQUIRED;
    }

LExit:
    ReleaseStr(sczDependent);
    ReleaseStr(sczFeatureBandVersion);
    LogUninitialize(TRUE);
    RegUninitialize();
    WiuUninitialize();
    return FAILED(hr) ? (int)hr : (int)dwExitCode;
}
