based on findings and usage in /examples.curl

std::sysWrite, std::sysReadLine : These will be augmented with console.writeln, console.write, console.readln -- no problem just a note

has "uses Lcurl;" outside the namespace, then the main() call is called from outside the namespace. How I see namespaces working, is that they are not classes, they are labels for boundries. So being inside a namespace wouldn't automatically disqualify you from auto-running, if your a name space in the entry file, why not just put the main() call at the botton of the namespace? Is there a reason that would be bad? I am open to a different perspective on this.

I only seen the one uses for main, I did not see any for Lcurl, because they are all in the Lcurl namespace. This is correct, just unusual to see for me.

Ya, we gotta fill out the standard libraries.

string? headerValue(Array<Header> headers, string name) { What is this string? to me that says nullable, but we dont have null... does that mean None able?

Im happy with how few notes I have... this is great work

