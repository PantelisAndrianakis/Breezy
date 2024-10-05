// Author: Pantelis Andrianakis
// Creation Date: October 5th 2024

using System.Text;
using System.Text.RegularExpressions;

namespace Breezy.Translators
{
	class HttpTranslator : MethodLibrary
	{
		public static string Process(string source)
		{
			bool foundReadString = false;
			bool foundReadBinary = false;

			// Support for random method names to avoid conflicts.
			string httpReadStringSuffix = "";
			string httpReadBinarySuffix = "";

			// Define regex patterns for Http.readString and Http.readBinary.
			string readStringPattern = @"Http\.(?i)readString\(([^;]+)\)";
			string readBinaryPattern = @"Http\.(?i)readBinary\(([^;]+)\)";

			// Replace Http.readString with httpReadString and track if found.
			source = Regex.Replace(source, readStringPattern, match =>
			{
				foundReadString = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("httpReadString("))
				{
					httpReadStringSuffix = GetRandomMethodIdentifier();
				}
				string url = match.Groups[1].Value;
				return $"httpReadString{httpReadStringSuffix}({url})";
			});

			// Replace Http.readBinary with httpReadBinary and track if found.
			source = Regex.Replace(source, readBinaryPattern, match =>
			{
				foundReadBinary = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("httpReadBinary("))
				{
					httpReadBinarySuffix = GetRandomMethodIdentifier();
				}
				string url = match.Groups[1].Value;
				return $"httpReadBinary{httpReadBinarySuffix}({url})";
			});

			// Add the necessary C++ methods if they are used.
			StringBuilder methods = new StringBuilder();

			// Append httpReadString method if it was found.
			if (foundReadString)
			{
				source = AddInclude(source, "string");
				source = AddInclude(source, "curl/curl.h");

				methods.AppendLine($"std::string httpReadString{httpReadStringSuffix}(const std::string& url)");
				methods.AppendLine("{");
				methods.AppendLine("\tCURL* curl;");
				methods.AppendLine("\tCURLcode res;");
				methods.AppendLine("\tstd::string response;");
				methods.AppendLine("\tcurl = curl_easy_init();");
				methods.AppendLine("\tif (curl)");
				methods.AppendLine("\t{");
				methods.AppendLine("\t\tcurl_easy_setopt(curl, CURLOPT_URL, url.c_str());");
				methods.AppendLine("\t\tcurl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t");
				methods.AppendLine("\t\t{");
				methods.AppendLine("\t\t\tstd::string* s = static_cast<std::string*>(userp);");
				methods.AppendLine("\t\t\tsize_t newLength = size * nmemb;");
				methods.AppendLine("\t\t\ts->append(static_cast<char*>(contents), newLength);");
				methods.AppendLine("\t\t\treturn newLength;");
				methods.AppendLine("\t\t});");
				methods.AppendLine("\t\tcurl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);");
				methods.AppendLine("\t\tres = curl_easy_perform(curl);");
				methods.AppendLine("\t\tcurl_easy_cleanup(curl);");
				methods.AppendLine("\t\tif (res != CURLE_OK)");
				methods.AppendLine("\t\t{");
				methods.AppendLine("\t\t\treturn \"\";");
				methods.AppendLine("\t\t}");
				methods.AppendLine("\t}");
				methods.AppendLine("\treturn response;");
				methods.AppendLine("}");
			}

			// Append httpReadBinary method if it was found.
			if (foundReadBinary)
			{
				source = AddInclude(source, "curl/curl.h");
				source = AddInclude(source, "vector");

				methods.AppendLine($"std::vector<char> httpReadBinary{httpReadBinarySuffix}(const std::string& url)");
				methods.AppendLine("{");
				methods.AppendLine("\tCURL* curl;");
				methods.AppendLine("\tCURLcode res;");
				methods.AppendLine("\tstd::vector<char> buffer;");
				methods.AppendLine("\tcurl = curl_easy_init();");
				methods.AppendLine("\tif (curl)");
				methods.AppendLine("\t{");
				methods.AppendLine("\t\tcurl_easy_setopt(curl, CURLOPT_URL, url.c_str());");
				methods.AppendLine("\t\tcurl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t");
				methods.AppendLine("\t\t{");
				methods.AppendLine("\t\t\tstd::vector<char>* buffer = static_cast<std::vector<char>*>(userp);");
				methods.AppendLine("\t\t\tsize_t totalSize = size * nmemb;");
				methods.AppendLine("\t\t\tbuffer->insert(buffer->end(), (char*)contents, (char*)contents + totalSize);");
				methods.AppendLine("\t\t\treturn totalSize;");
				methods.AppendLine("\t\t});");
				methods.AppendLine("\t\tcurl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);");
				methods.AppendLine("\t\tres = curl_easy_perform(curl);");
				methods.AppendLine("\t\tcurl_easy_cleanup(curl);");
				methods.AppendLine("\t\tif (res != CURLE_OK)");
				methods.AppendLine("\t\t{");
				methods.AppendLine("\t\t\treturn {};");
				methods.AppendLine("\t\t}");
				methods.AppendLine("\t}");
				methods.AppendLine("\treturn buffer;");
				methods.AppendLine("}");
			}

			// Parent class manages adding the additional methods.
			source = AddMethods(source, methods.ToString());

			return source;
		}
	}
}
